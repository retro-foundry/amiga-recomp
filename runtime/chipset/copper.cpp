// The Copper: a coprocessor that reads its own program from Chip RAM and
// writes custom registers in step with the beam (AMIGA_RECOMP.md 19).
//
// Copper lists are never treated as 68k code (rule 13). They are data the
// hardware interprets, and they are re-read from guest memory every frame, so
// a game that rewrites its list mid-frame behaves as it does on hardware.

#include "amiga_recomp/chipset.hpp"

namespace arecomp {

namespace {

// The Copper compares the beam against a masked target. The vertical mask
// always has its top bit set, so bit 8 of the line counter is never ignored.
bool beam_reached(u32 vpos, u32 hpos, u16 wait_vpos, u16 wait_hpos,
                  u16 mask_v, u16 mask_h) {
    const u32 beam = ((vpos & 0xff) << 8) | (hpos & 0xfe);
    const u32 target = (static_cast<u32>(wait_vpos) << 8) | wait_hpos;
    const u32 mask = (static_cast<u32>(mask_v) << 8) | mask_h;
    return (beam & mask) >= (target & mask);
}

} // namespace

void Chipset::copper_reset(u32 address) {
    copper_.pc = address;
    copper_.waiting = false;
    copper_.skip_next = false;
    copper_.cycle = 0;
}

void Chipset::copper_step() {
    if (copper_.cycle > 0) {
        --copper_.cycle;
        return;
    }

    if (copper_.waiting) {
        if (!beam_reached(vpos_, hpos_, copper_.wait_vpos, copper_.wait_hpos,
                          copper_.wait_mask_v, copper_.wait_mask_h))
            return;
        copper_.waiting = false;
    }

    const u16 first = memory_.peek16(copper_.pc);
    const u16 second = memory_.peek16(copper_.pc + 2);
    copper_.pc = (copper_.pc + 4) & 0x001ffffe;

    // Every Copper instruction occupies two of its slots.
    copper_.cycle = 1;

    if (copper_.skip_next) {
        copper_.skip_next = false;
        return;
    }

    if ((first & 1) == 0) {
        // MOVE: write an immediate to a custom register.
        const u32 offset = first & 0x1fe;
        // Without CDANG the Copper cannot reach the registers below $40.
        const bool cdang = (regs_[reg::COPCON >> 1] & 0x0002) != 0;
        if (offset < 0x40 && !cdang) return;
        write_register(offset, second, true);
        return;
    }

    const u16 wait_vpos = (first >> 8) & 0xff;
    const u16 wait_hpos = first & 0xfe;
    const u16 mask_v = static_cast<u16>(((second >> 8) & 0x7f) | 0x80);
    const u16 mask_h = second & 0xfe;
    const bool blitter_finished_disable = (second & 0x8000) != 0;

    if ((second & 1) == 0) {
        // WAIT. The canonical end-of-list entry, $FFFF/$FFFE, waits for a
        // line that never arrives, which parks the Copper until the next
        // frame restarts it from COP1LC.
        copper_.waiting = true;
        copper_.wait_vpos = wait_vpos;
        copper_.wait_hpos = wait_hpos;
        copper_.wait_mask_v = mask_v;
        copper_.wait_mask_h = mask_h;

        if (!blitter_finished_disable && blitter_.active) {
            // The wait also requires the blitter to finish; re-testing each
            // slot handles that without a separate state.
            return;
        }
        return;
    }

    // SKIP: skip the next instruction if the beam is already past the
    // position, and optionally if the blitter has finished.
    const bool reached = beam_reached(vpos_, hpos_, wait_vpos, wait_hpos, mask_v, mask_h);
    const bool blitter_ok = blitter_finished_disable || !blitter_.active;
    if (reached && blitter_ok) copper_.skip_next = true;
}

} // namespace arecomp
