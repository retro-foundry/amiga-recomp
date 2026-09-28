// The two 8520 CIAs (AMIGA_RECOMP.md 24).
//
// Games use these for timers, the joystick fire buttons, the keyboard line and
// floppy control. CIA-A raises level 2 (PORTS), CIA-B level 6 (EXTER).

#include "amiga_recomp/chipset.hpp"

namespace arecomp {

namespace {

enum : u32 {
    CIA_PRA = 0x0, CIA_PRB = 0x1, CIA_DDRA = 0x2, CIA_DDRB = 0x3,
    CIA_TALO = 0x4, CIA_TAHI = 0x5, CIA_TBLO = 0x6, CIA_TBHI = 0x7,
    CIA_TODLOW = 0x8, CIA_TODMID = 0x9, CIA_TODHI = 0xa,
    CIA_SDR = 0xc, CIA_ICR = 0xd, CIA_CRA = 0xe, CIA_CRB = 0xf,
};

enum : u8 {
    ICR_TA = 0x01,
    ICR_TB = 0x02,
    ICR_ALRM = 0x04,
    ICR_SP = 0x08,
    ICR_FLG = 0x10,
    ICR_SET = 0x80,
    ICR_IR = 0x80,   // in a read, "an interrupt is pending"
};

enum : u8 {
    CR_START = 0x01,
    CR_PBON = 0x02,
    CR_OUTMODE = 0x04,
    CR_RUNMODE = 0x08,   // one-shot when set
    CR_LOAD = 0x10,
};

} // namespace

u8 Chipset::cia_read(Cia& cia, u32 index, bool is_a) {
    switch (index) {
    case CIA_PRA:
        // CIA-A port A carries the two fire buttons, active low, plus the
        // power LED and the OVL line.
        return is_a ? cia.pra : cia.pra;
    case CIA_PRB: return cia.prb;
    case CIA_DDRA: return cia.ddra;
    case CIA_DDRB: return cia.ddrb;
    case CIA_TALO: return static_cast<u8>(cia.timer_a);
    case CIA_TAHI: return static_cast<u8>(cia.timer_a >> 8);
    case CIA_TBLO: return static_cast<u8>(cia.timer_b);
    case CIA_TBHI: return static_cast<u8>(cia.timer_b >> 8);
    case CIA_TODLOW: return static_cast<u8>(cia.tod);
    case CIA_TODMID: return static_cast<u8>(cia.tod >> 8);
    case CIA_TODHI: return static_cast<u8>(cia.tod >> 16);
    case CIA_SDR: return 0;
    case CIA_ICR: {
        // Reading the interrupt control register returns the pending sources
        // and clears them, which is how the handler acknowledges.
        const u8 data = cia.icr_data;
        const u8 value = static_cast<u8>(data | ((data & cia.icr_mask) ? ICR_IR : 0));
        cia.icr_data = 0;
        cia.irq = false;
        return value;
    }
    case CIA_CRA: return cia.cra;
    case CIA_CRB: return cia.crb;
    default: return 0xff;
    }
}

void Chipset::cia_write(Cia& cia, u32 index, u8 value, bool is_a) {
    switch (index) {
    case CIA_PRA: cia.pra = value; break;
    case CIA_PRB: cia.prb = value; break;
    case CIA_DDRA: cia.ddra = value; break;
    case CIA_DDRB: cia.ddrb = value; break;
    case CIA_TALO: cia.latch_a = static_cast<u16>((cia.latch_a & 0xff00) | value); break;
    case CIA_TAHI:
        cia.latch_a = static_cast<u16>((cia.latch_a & 0x00ff) | (value << 8));
        // Writing the high byte of a stopped timer loads it immediately.
        if (!(cia.cra & CR_START)) cia.timer_a = cia.latch_a;
        break;
    case CIA_TBLO: cia.latch_b = static_cast<u16>((cia.latch_b & 0xff00) | value); break;
    case CIA_TBHI:
        cia.latch_b = static_cast<u16>((cia.latch_b & 0x00ff) | (value << 8));
        if (!(cia.crb & CR_START)) cia.timer_b = cia.latch_b;
        break;
    case CIA_ICR:
        if (value & ICR_SET) cia.icr_mask = static_cast<u8>(cia.icr_mask | (value & 0x1f));
        else cia.icr_mask = static_cast<u8>(cia.icr_mask & ~(value & 0x1f));
        break;
    case CIA_CRA:
        cia.cra = value;
        if (value & CR_LOAD) cia.timer_a = cia.latch_a;
        break;
    case CIA_CRB:
        cia.crb = value;
        if (value & CR_LOAD) cia.timer_b = cia.latch_b;
        break;
    default:
        break;
    }
    (void)is_a;
}

void Chipset::cia_step() {
    auto tick = [&](Cia& cia, bool is_a) {
        bool fired = false;

        if (cia.cra & CR_START) {
            if (cia.timer_a == 0) {
                cia.timer_a = cia.latch_a;
                cia.icr_data = static_cast<u8>(cia.icr_data | ICR_TA);
                fired = true;
                // A one-shot timer stops itself after underflowing.
                if (cia.cra & CR_RUNMODE) cia.cra = static_cast<u8>(cia.cra & ~CR_START);
            } else {
                --cia.timer_a;
            }
        }

        // Timer B counts the same clock unless it is chained to timer A,
        // which games use for long delays.
        const bool chained = (cia.crb & 0x40) != 0;
        if ((cia.crb & CR_START) && !chained) {
            if (cia.timer_b == 0) {
                cia.timer_b = cia.latch_b;
                cia.icr_data = static_cast<u8>(cia.icr_data | ICR_TB);
                fired = true;
                if (cia.crb & CR_RUNMODE) cia.crb = static_cast<u8>(cia.crb & ~CR_START);
            } else {
                --cia.timer_b;
            }
        }

        if (fired && (cia.icr_data & cia.icr_mask) && !cia.irq) {
            cia.irq = true;
            raise_interrupt(is_a ? INTF_PORTS : INTF_EXTER);
        }
    };

    tick(cia_a_, true);
    tick(cia_b_, false);
}

} // namespace arecomp
