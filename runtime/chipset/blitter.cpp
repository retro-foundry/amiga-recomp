// The blitter (AMIGA_RECOMP.md 22).
//
// Blits are asynchronous (rule 14): starting one sets BBUSY, the work proceeds
// alongside translated CPU code, and completion raises the blitter interrupt.
// Games rely on that, either by waiting on BBUSY or by taking the interrupt.

#include "amiga_recomp/chipset.hpp"

namespace arecomp {

namespace {

constexpr u32 kDmaAddressMask = 0x001ffffe;

enum : u16 {
    BLTCON0_USEA = 0x0800,
    BLTCON0_USEB = 0x0400,
    BLTCON0_USEC = 0x0200,
    BLTCON0_USED = 0x0100,

    BLTCON1_EFE = 0x0010,   // exclusive fill
    BLTCON1_IFE = 0x0008,   // inclusive fill
    BLTCON1_FCI = 0x0004,   // fill carry in
    BLTCON1_DESC = 0x0002,  // descending
    BLTCON1_LINE = 0x0001,
};

} // namespace

u16 Chipset::blitter_minterm(u16 a, u16 b, u16 c) const {
    // The low byte of BLTCON0 is a truth table over A, B and C: bit n is the
    // output for the input combination n (AMIGA_RECOMP.md 22.2).
    const u16 lf = regs_[reg::BLTCON0 >> 1] & 0x00ff;
    u16 out = 0;
    if (lf & 0x01) out |= static_cast<u16>(~a & ~b & ~c);
    if (lf & 0x02) out |= static_cast<u16>(~a & ~b & c);
    if (lf & 0x04) out |= static_cast<u16>(~a & b & ~c);
    if (lf & 0x08) out |= static_cast<u16>(~a & b & c);
    if (lf & 0x10) out |= static_cast<u16>(a & ~b & ~c);
    if (lf & 0x20) out |= static_cast<u16>(a & ~b & c);
    if (lf & 0x40) out |= static_cast<u16>(a & b & ~c);
    if (lf & 0x80) out |= static_cast<u16>(a & b & c);
    return out;
}

void Chipset::blitter_start() {
    const u16 con0 = regs_[reg::BLTCON0 >> 1];
    const u16 con1 = regs_[reg::BLTCON1 >> 1];
    const u16 size = regs_[reg::BLTSIZE >> 1];

    Blitter& b = blitter_;
    b = Blitter{};

    // A zero field means the maximum, not nothing.
    b.width = size & 0x3f;
    if (b.width == 0) b.width = 64;
    b.height = (size >> 6) & 0x3ff;
    if (b.height == 0) b.height = 1024;

    b.descending = (con1 & BLTCON1_DESC) != 0;
    b.line_mode = (con1 & BLTCON1_LINE) != 0;
    b.fill_enabled = (con1 & (BLTCON1_EFE | BLTCON1_IFE)) != 0;
    b.fill_carry = (con1 & BLTCON1_FCI) != 0;
    b.a_shift = (con0 >> 12) & 0x0f;
    b.b_shift = (con1 >> 12) & 0x0f;

    b.pt[0] = ((static_cast<u32>(regs_[reg::BLTAPTH >> 1]) << 16) |
               regs_[reg::BLTAPTL >> 1]) & kDmaAddressMask;
    b.pt[1] = ((static_cast<u32>(regs_[reg::BLTBPTH >> 1]) << 16) |
               regs_[reg::BLTBPTL >> 1]) & kDmaAddressMask;
    b.pt[2] = ((static_cast<u32>(regs_[reg::BLTCPTH >> 1]) << 16) |
               regs_[reg::BLTCPTL >> 1]) & kDmaAddressMask;
    b.pt[3] = ((static_cast<u32>(regs_[reg::BLTDPTH >> 1]) << 16) |
               regs_[reg::BLTDPTL >> 1]) & kDmaAddressMask;

    b.mod[0] = static_cast<s16>(regs_[reg::BLTAMOD >> 1]);
    b.mod[1] = static_cast<s16>(regs_[reg::BLTBMOD >> 1]);
    b.mod[2] = static_cast<s16>(regs_[reg::BLTCMOD >> 1]);
    b.mod[3] = static_cast<s16>(regs_[reg::BLTDMOD >> 1]);

    b.dat[0] = regs_[reg::BLTADAT >> 1];
    b.dat[1] = regs_[reg::BLTBDAT >> 1];
    b.dat[2] = regs_[reg::BLTCDAT >> 1];

    b.active = true;
    b.any_nonzero = false;
    b.x = 0;
    b.y = 0;
    b.hold_a = 0;
    b.hold_b = 0;
}

void Chipset::blitter_run() {
    Blitter& b = blitter_;
    if (!b.active) return;

    if (b.line_mode) {
        // Line mode draws one pixel per step using the Bresenham state the
        // game set up in the A channel: BLTAPT is the error accumulator,
        // BLTAMOD and BLTBMOD the two increments (AMIGA_RECOMP.md 22.3).
        const u16 con1 = regs_[reg::BLTCON1 >> 1];
        const bool sign = (con1 & 0x0040) != 0;      // SIGN
        const bool sud = (con1 & 0x0010) != 0;
        const bool sul = (con1 & 0x0020) != 0;
        const bool aul = (con1 & 0x0080) != 0;

        const u32 bit = 15 - (b.a_shift & 0x0f);
        const u16 pattern = regs_[reg::BLTBDAT >> 1];
        const bool draw = ((pattern >> (b.y & 15)) & 1) != 0;

        if (draw) {
            const u16 existing = memory_.peek16(b.pt[3]);
            const u16 value = static_cast<u16>(existing | (1u << bit));
            memory_.poke16(b.pt[3], value);
            if (value != 0) b.any_nonzero = true;
        }

        // Advance along the major axis, and along the minor axis when the
        // error accumulator says so.
        s32 error = static_cast<s32>(static_cast<s16>(b.pt[0] & 0xffff));
        u32 step_major = sud ? static_cast<u32>(b.mod[3]) : 2u;
        if (!sign) {
            error += b.mod[0];
            // Minor axis step.
            if (sul) b.pt[3] -= static_cast<u32>(step_major);
            else b.pt[3] += static_cast<u32>(step_major);
        } else {
            error += b.mod[1];
        }
        (void)aul;
        b.pt[0] = (b.pt[0] & 0xffff0000u) | (static_cast<u32>(error) & 0xffffu);
        regs_[reg::BLTCON1 >> 1] = static_cast<u16>(error < 0 ? (con1 | 0x0040)
                                                              : (con1 & ~0x0040));

        if (++b.y >= b.height) {
            b.active = false;
            raise_interrupt(INTF_BLIT);
        }
        return;
    }

    // --- area mode: one word per step ------------------------------------
    const u16 con0 = regs_[reg::BLTCON0 >> 1];
    const s32 direction = b.descending ? -2 : 2;

    u16 a = b.dat[0];
    u16 bb = b.dat[1];
    u16 c = b.dat[2];

    if (con0 & BLTCON0_USEA) {
        a = memory_.peek16(b.pt[0]);
        b.pt[0] = (b.pt[0] + static_cast<u32>(direction)) & kDmaAddressMask;
    }
    if (con0 & BLTCON0_USEB) {
        bb = memory_.peek16(b.pt[1]);
        b.pt[1] = (b.pt[1] + static_cast<u32>(direction)) & kDmaAddressMask;
    }
    if (con0 & BLTCON0_USEC) {
        c = memory_.peek16(b.pt[2]);
        b.pt[2] = (b.pt[2] + static_cast<u32>(direction)) & kDmaAddressMask;
    }

    // The first and last words of every row are masked, so a blit can start
    // and end part way through a word.
    if (b.x == 0) a &= regs_[reg::BLTAFWM >> 1];
    if (b.x == b.width - 1) a &= regs_[reg::BLTALWM >> 1];

    // Barrel shift A and B across the word boundary.
    u16 shifted_a, shifted_b;
    if (b.descending) {
        shifted_a = static_cast<u16>(((static_cast<u32>(a) << 16) | b.hold_a) >>
                                     (16 - b.a_shift));
        shifted_b = static_cast<u16>(((static_cast<u32>(bb) << 16) | b.hold_b) >>
                                     (16 - b.b_shift));
        if (b.a_shift == 0) shifted_a = a;
        if (b.b_shift == 0) shifted_b = bb;
    } else {
        shifted_a = static_cast<u16>(((static_cast<u32>(b.hold_a) << 16) | a) >> b.a_shift);
        shifted_b = static_cast<u16>(((static_cast<u32>(b.hold_b) << 16) | bb) >> b.b_shift);
    }
    b.hold_a = a;
    b.hold_b = bb;

    u16 d = blitter_minterm(shifted_a, shifted_b, c);

    if (b.fill_enabled) {
        // Fill scans each word from the rightmost pixel (bit 0) leftwards,
        // toggling the carry at every set bit.
        const bool inclusive = (regs_[reg::BLTCON1 >> 1] & BLTCON1_IFE) != 0;
        u16 out = 0;
        for (unsigned i = 0; i < 16; ++i) {
            const bool bit_set = ((d >> i) & 1) != 0;
            if (inclusive) {
                if (bit_set || b.fill_carry) out |= static_cast<u16>(1u << i);
                if (bit_set) b.fill_carry = !b.fill_carry;
            } else {
                if (bit_set) b.fill_carry = !b.fill_carry;
                if (b.fill_carry) out |= static_cast<u16>(1u << i);
            }
        }
        d = out;
    }

    if (d != 0) b.any_nonzero = true;

    if (con0 & BLTCON0_USED) {
        memory_.poke16(b.pt[3], d);
        b.pt[3] = (b.pt[3] + static_cast<u32>(direction)) & kDmaAddressMask;
    }

    if (++b.x >= b.width) {
        b.x = 0;
        // Modulos are applied at the end of every row, in the blit's own
        // direction.
        const s32 sign = b.descending ? -1 : 1;
        if (con0 & BLTCON0_USEA) b.pt[0] = (b.pt[0] + static_cast<u32>(sign * b.mod[0])) & kDmaAddressMask;
        if (con0 & BLTCON0_USEB) b.pt[1] = (b.pt[1] + static_cast<u32>(sign * b.mod[1])) & kDmaAddressMask;
        if (con0 & BLTCON0_USEC) b.pt[2] = (b.pt[2] + static_cast<u32>(sign * b.mod[2])) & kDmaAddressMask;
        if (con0 & BLTCON0_USED) b.pt[3] = (b.pt[3] + static_cast<u32>(sign * b.mod[3])) & kDmaAddressMask;

        b.hold_a = 0;
        b.hold_b = 0;
        b.fill_carry = (regs_[reg::BLTCON1 >> 1] & BLTCON1_FCI) != 0;

        if (++b.y >= b.height) {
            b.active = false;
            // Write the final pointers back, which is what a game reads if it
            // inspects them after the blit.
            regs_[reg::BLTAPTH >> 1] = static_cast<u16>(b.pt[0] >> 16);
            regs_[reg::BLTAPTL >> 1] = static_cast<u16>(b.pt[0]);
            regs_[reg::BLTDPTH >> 1] = static_cast<u16>(b.pt[3] >> 16);
            regs_[reg::BLTDPTL >> 1] = static_cast<u16>(b.pt[3]);
            raise_interrupt(INTF_BLIT);
        }
    }
}

} // namespace arecomp
