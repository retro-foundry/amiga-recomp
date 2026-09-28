// Chipset core: register decode, DMA control, interrupts and the beam.
#include "amiga_recomp/chipset.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace arecomp {

namespace {

// Chip RAM is what DMA can see. Agnus drives 21 address bits on an A500 with
// 1 MiB of Chip RAM; pointers are always even.
constexpr u32 kDmaAddressMask = 0x001ffffe;

u32 combine_pointer(u16 high, u16 low) {
    return ((static_cast<u32>(high) << 16) | low) & kDmaAddressMask;
}

} // namespace

const char* custom_register_name(u32 offset) noexcept {
    switch (offset & 0x1fe) {
    case reg::DMACONR: return "DMACONR";
    case reg::VPOSR: return "VPOSR";
    case reg::VHPOSR: return "VHPOSR";
    case reg::JOY0DAT: return "JOY0DAT";
    case reg::JOY1DAT: return "JOY1DAT";
    case reg::CLXDAT: return "CLXDAT";
    case reg::ADKCONR: return "ADKCONR";
    case reg::POTGOR: return "POTGOR";
    case reg::INTENAR: return "INTENAR";
    case reg::INTREQR: return "INTREQR";
    case reg::DSKPTH: return "DSKPTH";
    case reg::DSKLEN: return "DSKLEN";
    case reg::VPOSW: return "VPOSW";
    case reg::VHPOSW: return "VHPOSW";
    case reg::COPCON: return "COPCON";
    case reg::POTGO: return "POTGO";
    case reg::BLTCON0: return "BLTCON0";
    case reg::BLTCON1: return "BLTCON1";
    case reg::BLTAFWM: return "BLTAFWM";
    case reg::BLTALWM: return "BLTALWM";
    case reg::BLTCPTH: return "BLTCPTH";
    case reg::BLTBPTH: return "BLTBPTH";
    case reg::BLTAPTH: return "BLTAPTH";
    case reg::BLTDPTH: return "BLTDPTH";
    case reg::BLTSIZE: return "BLTSIZE";
    case reg::BLTCMOD: return "BLTCMOD";
    case reg::BLTBMOD: return "BLTBMOD";
    case reg::BLTAMOD: return "BLTAMOD";
    case reg::BLTDMOD: return "BLTDMOD";
    case reg::BLTCDAT: return "BLTCDAT";
    case reg::BLTBDAT: return "BLTBDAT";
    case reg::BLTADAT: return "BLTADAT";
    case reg::COP1LCH: return "COP1LCH";
    case reg::COP1LCL: return "COP1LCL";
    case reg::COP2LCH: return "COP2LCH";
    case reg::COP2LCL: return "COP2LCL";
    case reg::COPJMP1: return "COPJMP1";
    case reg::COPJMP2: return "COPJMP2";
    case reg::DIWSTRT: return "DIWSTRT";
    case reg::DIWSTOP: return "DIWSTOP";
    case reg::DDFSTRT: return "DDFSTRT";
    case reg::DDFSTOP: return "DDFSTOP";
    case reg::DMACON: return "DMACON";
    case reg::INTENA: return "INTENA";
    case reg::INTREQ: return "INTREQ";
    case reg::ADKCON: return "ADKCON";
    case reg::BPLCON0: return "BPLCON0";
    case reg::BPLCON1: return "BPLCON1";
    case reg::BPLCON2: return "BPLCON2";
    case reg::BPL1MOD: return "BPL1MOD";
    case reg::BPL2MOD: return "BPL2MOD";
    default: break;
    }
    if (offset >= reg::COLOR00 && offset < reg::COLOR00 + 64) return "COLORxx";
    if (offset >= reg::BPL1PTH && offset < reg::BPL1PTH + 24) return "BPLxPT";
    if (offset >= reg::SPR0PTH && offset < reg::SPR0PTH + 32) return "SPRxPT";
    if (offset >= reg::SPR0POS && offset < reg::SPR0POS + 64) return "SPRxCTL";
    if (offset >= reg::AUD0LCH && offset < reg::AUD0LCH + 64) return "AUDxxx";
    if (offset >= reg::BPL1DAT && offset < reg::BPL1DAT + 12) return "BPLxDAT";
    return nullptr;
}

Chipset::Chipset(GuestMemory& memory, Runtime& runtime, ChipsetConfig config)
    : memory_(memory), runtime_(runtime), config_(config) {
    lines_per_frame_ = config_.standard == VideoStandard::Pal ? kLinesPerFramePal
                                                              : kLinesPerFrameNtsc;
    colour_clocks_per_line_ = config_.standard == VideoStandard::Pal
                                  ? kColourClocksPerLinePal
                                  : kColourClocksPerLineNtsc;

    // Power-on values that matter: no DMA, no interrupts, sane display window.
    regs_[reg::DIWSTRT >> 1] = 0x2c81;
    regs_[reg::DIWSTOP >> 1] = 0xf4c1;
    regs_[reg::DDFSTRT >> 1] = 0x0038;
    regs_[reg::DDFSTOP >> 1] = 0x00d0;
    for (auto& channel : audio_) channel.length = 1;

    recompute_display_window();
    begin_frame();
}

void Chipset::map(GuestMemory& memory) {
    memory.map_device(kCustomBase, kCustomSize, this, RegionKind::Custom, "custom");
    memory.map_device(kCiaBase, kCiaSize, this, RegionKind::Cia, "cia");
}

// ---------------------------------------------------------------------------
// Bus interface
// ---------------------------------------------------------------------------

u16 Chipset::read16(u32 addr) {
    if (addr >= kCiaBase && addr < kCiaBase + kCiaSize) {
        // A word access to CIA space reads two different bytes; games use byte
        // accesses, but be consistent rather than inventing a word register.
        const u16 high = read8(addr);
        const u16 low = read8(addr + 1);
        return static_cast<u16>((high << 8) | low);
    }
    return read_register(addr & 0x1fe);
}

u8 Chipset::read8(u32 addr) {
    if (addr >= kCiaBase && addr < kCiaBase + kCiaSize) {
        // CIA-A is selected by A12 low, CIA-B by A13 low; the register index
        // is in A8-A12 (AMIGA.md, CIA addressing).
        const u32 index = (addr >> 8) & 0x0f;
        if ((addr & 0x1000) == 0) return cia_read(cia_a_, index, true);
        if ((addr & 0x2000) == 0) return cia_read(cia_b_, index, false);
        return 0xff;
    }
    const u16 word = read_register(addr & 0x1fe);
    return (addr & 1) ? static_cast<u8>(word) : static_cast<u8>(word >> 8);
}

void Chipset::write16(u32 addr, u16 value) {
    if (addr >= kCiaBase && addr < kCiaBase + kCiaSize) {
        write8(addr, static_cast<u8>(value >> 8));
        write8(addr + 1, static_cast<u8>(value));
        return;
    }
    write_register(addr & 0x1fe, value, false);
}

void Chipset::write8(u32 addr, u8 value) {
    if (addr >= kCiaBase && addr < kCiaBase + kCiaSize) {
        const u32 index = (addr >> 8) & 0x0f;
        if ((addr & 0x1000) == 0) cia_write(cia_a_, index, value, true);
        else if ((addr & 0x2000) == 0) cia_write(cia_b_, index, value, false);
        return;
    }
    // A byte write to a custom register writes the whole word on real
    // hardware, with the byte duplicated into both halves.
    write_register(addr & 0x1fe, static_cast<u16>((value << 8) | value), false);
}

// ---------------------------------------------------------------------------
// Register file
// ---------------------------------------------------------------------------

u16 Chipset::read_register(u32 offset) {
    switch (offset) {
    case reg::DMACONR:
        return static_cast<u16>(regs_[reg::DMACON >> 1] |
                                (blitter_.active ? DMAF_BLTDONE : 0) |
                                (blitter_.any_nonzero ? 0 : DMAF_BLTNZERO));
    case reg::VPOSR:
        // Bit 15 is the long-frame flag; bit 0 is vpos bit 8.
        return static_cast<u16>(((vpos_ >> 8) & 1) | 0x8000);
    case reg::VHPOSR:
        return static_cast<u16>(((vpos_ & 0xff) << 8) | (hpos_ & 0xff));
    case reg::JOY0DAT: return joy_dat_[0];
    case reg::JOY1DAT: return joy_dat_[1];
    case reg::ADKCONR: return regs_[reg::ADKCON >> 1];
    case reg::INTENAR: return regs_[reg::INTENA >> 1];
    case reg::INTREQR: return regs_[reg::INTREQ >> 1];
    case reg::CLXDAT: {
        const u16 value = regs_[reg::CLXDAT >> 1];
        regs_[reg::CLXDAT >> 1] = 0;   // cleared by reading
        return value;
    }
    case reg::POTGOR: return 0xff00;   // no buttons pulled low
    case reg::SERDATR: return 0x2000;  // transmit buffer empty
    case reg::DSKBYTR: return 0;
    default:
        return regs_[offset >> 1];
    }
}

void Chipset::write_register(u32 offset, u16 value, bool from_copper) {
    if (config_.log_register_writes) {
        log_.push_back({now_, offset, value, static_cast<u16>(vpos_),
                        static_cast<u16>(hpos_), from_copper});
    }

    // The four set/clear registers. Never modelled as plain assignment
    // (AMIGA_RECOMP.md 18).
    auto set_clear = [&](u32 target, u16 writable_mask) {
        u16& r = regs_[target >> 1];
        const u16 bits = value & writable_mask;
        if (value & 0x8000) r = static_cast<u16>(r | bits);
        else r = static_cast<u16>(r & ~bits);
    };

    // A targeted trace of the registers that decide whether interrupts and DMA
    // happen at all. These are the first things to check when a game sits in a
    // wait loop (AMIGA_RECOMP.md 33).
    static const bool trace_control = std::getenv("ARECOMP_REG_TRACE") != nullptr;
    if (trace_control && (offset == reg::INTENA || offset == reg::INTREQ ||
                          offset == reg::DMACON || offset == reg::COP1LCH ||
                          offset == reg::COP1LCL || offset == reg::COPJMP1 ||
                          offset == reg::BPLCON0)) {
        std::fprintf(stderr, "[reg] %s <- $%04x  (vpos %u, %s)\n",
                     custom_register_name(offset), value, vpos_,
                     from_copper ? "copper" : "cpu");
    }

    switch (offset) {
    case reg::DMACON:
        set_clear(reg::DMACON, DMAF_WRITABLE);
        copper_.enabled = (regs_[reg::DMACON >> 1] & (DMAF_MASTER | DMAF_COPPER)) ==
                          (DMAF_MASTER | DMAF_COPPER);
        return;

    case reg::INTENA:
        set_clear(reg::INTENA, 0x7fff);
        update_interrupts();
        return;

    case reg::INTREQ:
        set_clear(reg::INTREQ, 0x7fff);
        update_interrupts();
        return;

    case reg::ADKCON:
        set_clear(reg::ADKCON, 0x7fff);
        return;

    case reg::COPJMP1:
        copper_reset(combine_pointer(regs_[reg::COP1LCH >> 1], regs_[reg::COP1LCL >> 1]));
        return;
    case reg::COPJMP2:
        copper_reset(combine_pointer(regs_[reg::COP2LCH >> 1], regs_[reg::COP2LCL >> 1]));
        return;

    case reg::BLTSIZE:
        regs_[offset >> 1] = value;
        blitter_start();
        return;

    case reg::DSKLEN:
        regs_[offset >> 1] = value;
        disk_start_dma();
        return;

    case reg::DIWSTRT:
    case reg::DIWSTOP:
    case reg::DDFSTRT:
    case reg::DDFSTOP:
    case reg::BPLCON0:
    case reg::BPLCON1:
        regs_[offset >> 1] = value;
        recompute_display_window();
        return;

    case reg::VPOSW:
    case reg::VHPOSW:
        // Writing the beam position is possible on real hardware but nothing
        // a game does usefully; record it and move on.
        regs_[offset >> 1] = value;
        return;

    default:
        break;
    }

    // Bitplane pointers: the high write starts the address, the low completes
    // it. Both halves are latched into the working pointer immediately, which
    // is what games rely on when they set pointers during the vertical blank.
    if (offset >= reg::BPL1PTH && offset < reg::BPL1PTH + 24) {
        regs_[offset >> 1] = value;
        const u32 plane = (offset - reg::BPL1PTH) / 4;
        bpl_pointer_[plane] = combine_pointer(regs_[(reg::BPL1PTH + plane * 4) >> 1],
                                              regs_[(reg::BPL1PTL + plane * 4) >> 1]);
        return;
    }
    if (offset >= reg::SPR0PTH && offset < reg::SPR0PTH + 32) {
        regs_[offset >> 1] = value;
        const u32 index = (offset - reg::SPR0PTH) / 4;
        sprites_[index].pointer = combine_pointer(regs_[(reg::SPR0PTH + index * 4) >> 1],
                                                  regs_[(reg::SPR0PTL + index * 4) >> 1]);
        return;
    }
    if (offset >= reg::SPR0POS && offset < reg::SPR0POS + 64) {
        regs_[offset >> 1] = value;
        const u32 index = (offset - reg::SPR0POS) / 8;
        Sprite& sprite = sprites_[index];
        switch ((offset - reg::SPR0POS) % 8) {
        case 0: sprite.pos = value; break;
        case 2:
            sprite.ctl = value;
            sprite.armed = false;   // writing SPRxCTL disarms the sprite
            break;
        case 4: sprite.data_a = value; sprite.armed = true; break;
        case 6: sprite.data_b = value; break;
        default: break;
        }
        if ((offset - reg::SPR0POS) % 8 <= 2) {
            sprite.start_line = static_cast<u32>(((sprite.ctl & 0x04) << 6) |
                                                 ((sprite.pos >> 8) & 0xff));
            sprite.stop_line = static_cast<u32>(((sprite.ctl & 0x02) << 7) |
                                                ((sprite.ctl >> 8) & 0xff));
            sprite.x = static_cast<u32>(((sprite.pos & 0xff) << 1) | (sprite.ctl & 1));
            sprite.attached = (sprite.ctl & 0x80) != 0;
        }
        return;
    }
    if (offset >= reg::AUD0LCH && offset < reg::AUD0LCH + 64) {
        regs_[offset >> 1] = value;
        const u32 channel = (offset - reg::AUD0LCH) / 16;
        AudioChannel& audio = audio_[channel];
        switch ((offset - reg::AUD0LCH) % 16) {
        case 0: case 2:
            audio.location = combine_pointer(regs_[(reg::AUD0LCH + channel * 16) >> 1],
                                             regs_[(reg::AUD0LCL + channel * 16) >> 1]);
            break;
        case 4: audio.length = value ? value : 1; break;
        case 6: audio.period = value ? value : 1; break;
        case 8: audio.volume = value & 0x7f; break;
        case 10: audio.sample = value; break;
        default: break;
        }
        return;
    }

    regs_[offset >> 1] = value;
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------

void Chipset::raise_interrupt(u16 bits) {
    regs_[reg::INTREQ >> 1] = static_cast<u16>(regs_[reg::INTREQ >> 1] | bits);
    update_interrupts();
}

void Chipset::update_interrupts() {
    const u16 enable = regs_[reg::INTENA >> 1];
    const u16 request = regs_[reg::INTREQ >> 1];
    if (!(enable & INTF_INTEN)) {
        pending_level_ = 0;
        return;
    }
    const u16 active = static_cast<u16>(enable & request & 0x3fff);
    u8 level = 0;
    for (unsigned bit = 0; bit < 14; ++bit) {
        if (active & (1u << bit)) level = std::max(level, interrupt_level_for_bit(bit));
    }
    pending_level_ = level;
}

u32 Chipset::acknowledge_interrupt(u8 level) {
    // The Amiga autovectors: level n takes vector 24 + n. Acknowledging does
    // not clear INTREQ; the handler is expected to write it.
    return 24u + level;
}

// ---------------------------------------------------------------------------
// Beam and scheduling
// ---------------------------------------------------------------------------

MasterTick Chipset::next_event_time() const {
    // The chipset has something to do on every colour clock, so there is no
    // useful way to skip ahead. Returning the next colour clock keeps the CPU
    // and the hardware in step, which is the whole point (rule 19).
    return now_ + kCpuCyclesPerColourClock;
}

void Chipset::run_until(MasterTick target) {
    while (now_ + kCpuCyclesPerColourClock <= target) {
        step_colour_clock();
        now_ += kCpuCyclesPerColourClock;
    }
}

void Chipset::step_colour_clock() {
    const u16 dmacon = regs_[reg::DMACON >> 1];
    const bool master = (dmacon & DMAF_MASTER) != 0;

    // The Copper owns the odd colour clocks of each line.
    if (master && (dmacon & DMAF_COPPER) && (hpos_ & 1)) copper_step();

    // The blitter uses whatever DMA slots are left.
    if (master && (dmacon & DMAF_BLITTER) && blitter_.active) blitter_run();

    disk_index_pulse(1);

    if (master && (dmacon & DMAF_DISK)) disk_step();

    if (master && (dmacon & DMAF_RASTER)) fetch_bitplane_words();
    shift_out_pixels();

    audio_step();

    if (++cia_divider_ >= kCiaTicksPerColourClock) {
        cia_divider_ = 0;
        cia_step();
    }

    if (++hpos_ >= colour_clocks_per_line_) {
        hpos_ = 0;
        end_line();
        if (++vpos_ >= lines_per_frame_) {
            vpos_ = 0;
            ++frames_;
            if (on_frame_) on_frame_(frame_);
            begin_frame();
        }
        begin_line();
    }
}

void Chipset::begin_frame() {
    // Bitplane and sprite pointers are reloaded from the registers once per
    // frame, which is why games rewrite them during the vertical blank.
    for (u32 plane = 0; plane < 8; ++plane) {
        bpl_pointer_[plane] = combine_pointer(regs_[(reg::BPL1PTH + plane * 4) >> 1],
                                              regs_[(reg::BPL1PTL + plane * 4) >> 1]);
    }
    for (u32 i = 0; i < 8; ++i) {
        sprites_[i].pointer = combine_pointer(regs_[(reg::SPR0PTH + i * 4) >> 1],
                                              regs_[(reg::SPR0PTL + i * 4) >> 1]);
    }

    // Fix the rendered rectangle for the whole frame.
    recompute_display_window();

    // Widening the window to the left means starting each row further back in
    // the game's own bitmap. Without this the extra columns on the left would
    // show data belonging further right, and the picture would tear.
    if (config_.widescreen.enabled && display_.extra_left_words > 0) {
        const u32 back = display_.extra_left_words * 2;
        for (u32 plane = 0; plane < 8; ++plane)
            bpl_pointer_[plane] = (bpl_pointer_[plane] - back) & kDmaAddressMask;
    }

    const u32 extra_left = display_.extra_left_px;
    const u32 extra_right = display_.extra_right_px;

    u32 x0 = display_.diw_start_x > extra_left ? display_.diw_start_x - extra_left : 0;
    u32 x1 = std::min(display_.diw_stop_x + extra_right, kLoresPixelsPerLine);
    if (x1 <= x0) x1 = x0 + 1;

    // Record what the view actually gained, so a port can tell the difference
    // between what it asked for and what the raster allowed.
    display_.extra_left_px = display_.diw_start_x - x0;
    display_.extra_right_px = x1 - std::min(display_.diw_stop_x, x1);

    display_.view_x0 = x0;
    display_.view_y0 = display_.diw_start_y;
    display_.view_w = x1 - x0;
    display_.view_h = display_.diw_stop_y > display_.diw_start_y
                          ? display_.diw_stop_y - display_.diw_start_y
                          : 1;

    // Two framebuffer columns per lores pixel, so hires loses nothing.
    const u32 fb_width = display_.view_w * 2;
    if (frame_.width != fb_width || frame_.height != display_.view_h)
        frame_.resize(fb_width, display_.view_h);

    // Vertical blank interrupt. Raised at the top of the frame, which is what
    // every Amiga game's main loop waits for.
    raise_interrupt(INTF_VERTB);

    // Copper restarts from COP1LC every frame.
    copper_reset(combine_pointer(regs_[reg::COP1LCH >> 1], regs_[reg::COP1LCL >> 1]));

    begin_line();
}

void Chipset::begin_line() {
    display_.fetched_words = 0;
    display_.fetching = false;
    display_.out_x = 0;
    last_index_ = 0;
    last_index_valid_ = false;
    for (u32 plane = 0; plane < 8; ++plane) {
        shift_[plane] = 0;
        shift_count_[plane] = 0;
    }
    latch_sprite_dma();
}

void Chipset::end_line() {
    // Apply the bitplane modulos once per line, on the lines that fetched.
    const bool displaying = vpos_ >= display_.diw_start_y && vpos_ < display_.diw_stop_y;
    if (!displaying || display_.fetched_words == 0) return;

    const s16 mod1 = static_cast<s16>(regs_[reg::BPL1MOD >> 1]);
    const s16 mod2 = static_cast<s16>(regs_[reg::BPL2MOD >> 1]);
    // Widening the fetch window advances each pointer further per line. To
    // keep the row stride the game intended, take that back out of the
    // modulo; see docs/widescreen.md.
    const s32 correction =
        config_.widescreen.compensate_modulo
            ? -static_cast<s32>(display_.extra_words) * 2
            : 0;

    for (u32 plane = 0; plane < display_.planes; ++plane) {
        const s32 modulo = ((plane & 1) == 0 ? mod1 : mod2) + correction;
        bpl_pointer_[plane] = (bpl_pointer_[plane] + static_cast<u32>(modulo)) & kDmaAddressMask;
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void Chipset::set_joystick(unsigned port, bool up, bool down, bool left, bool right,
                           bool fire) {
    if (port > 1) return;
    // JOYxDAT encodes the directions as a pair of gray-coded counters. Games
    // read bit 1 as "vertical" and bit 9 as "horizontal", with bit 0 and bit 8
    // exclusive-ored in to give direction (AMIGA.md, input).
    u16 value = 0;
    if (right) value |= 0x0002;
    if (left) value |= 0x0200;
    if (down) value |= 0x0001;
    if (up) value |= 0x0100;
    // The exclusive-or that turns the counter pair into a direction.
    if (down) value ^= 0x0002;
    if (up) value ^= 0x0200;
    joy_dat_[port] = value;
    joy_fire_[port] = fire;

    // Fire buttons appear as active-low bits in CIA-A port A.
    u8 pra = 0xff;
    if (joy_fire_[0]) pra = static_cast<u8>(pra & ~0x40);
    if (joy_fire_[1]) pra = static_cast<u8>(pra & ~0x80);
    cia_a_.pra = pra;
}

void Chipset::set_key(u8 raw_code, bool pressed) {
    // The keyboard arrives as a serial stream into CIA-A. Games read the raw
    // code from SDR and acknowledge with a handshake; raising PORTS is enough
    // for the handler to run.
    cia_a_.icr_data = static_cast<u8>(cia_a_.icr_data | 0x08);
    (void)raw_code;
    (void)pressed;
    if (cia_a_.icr_mask & 0x08) raise_interrupt(INTF_PORTS);
}

int Chipset::slack_words(unsigned playfield) const {
    // How many extra words per line the game's own modulo leaves. A bitmap
    // wider than the display window shows up here as positive slack.
    const s16 modulo = static_cast<s16>(playfield == 0 ? regs_[reg::BPL1MOD >> 1]
                                                       : regs_[reg::BPL2MOD >> 1]);
    return modulo / 2;
}

} // namespace arecomp
