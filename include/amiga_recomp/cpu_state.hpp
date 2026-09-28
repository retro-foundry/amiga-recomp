// Explicit MC68000 guest CPU state (AMIGA_RECOMP.md 6).
//
// Deviation from the literal struct in the specification, documented in
// docs/architecture.md: the condition codes are held as separate 0/1 bytes
// rather than packed into `sr`, with sr()/set_sr() doing the packing. The
// guest state stays fully explicit and deterministic -- it is never the host
// CPU's flags -- but translated code does not pay bitfield insert/extract on
// every arithmetic instruction, and the flag helpers are far easier to get
// right and to unit-test.
#pragma once

#include "amiga_recomp/types.hpp"

namespace arecomp {

// Status register bit positions, for the packed form.
enum : u16 {
    SR_C = 0x0001,
    SR_V = 0x0002,
    SR_Z = 0x0004,
    SR_N = 0x0008,
    SR_X = 0x0010,
    SR_INT_MASK = 0x0700,
    SR_S = 0x2000,
    SR_T0 = 0x4000, // 68020+, decoded but not honoured on 68000
    SR_T1 = 0x8000,
    SR_CCR_MASK = 0x001f,
    // Bits a 68000 actually implements. T0 is not one of them.
    SR_IMPLEMENTED_68000 = 0xa71f,
};

struct M68kState {
    u32 d[8]{};
    u32 a[8]{};

    u32 pc = 0;

    // Condition codes, each 0 or 1.
    u8 x = 0;
    u8 n = 0;
    u8 z = 0;
    u8 v = 0;
    u8 c = 0;

    u8 int_mask = 7;  // I2:I0
    bool s = true;    // supervisor
    bool t1 = false;  // trace on any instruction

    // The inactive stack pointer. a[7] always holds the *active* one.
    u32 usp = 0;
    u32 ssp = 0;

    u64 cycles = 0;

    // Set when the CPU is halted by a double fault or by the test harness
    // sentinel. The dispatcher stops when this is true.
    bool halted = false;

    // Set by STOP; cleared when an interrupt of sufficient priority arrives.
    bool stopped = false;

    [[nodiscard]] u8 ccr() const noexcept {
        return static_cast<u8>((x ? SR_X : 0) | (n ? SR_N : 0) | (z ? SR_Z : 0) |
                               (v ? SR_V : 0) | (c ? SR_C : 0));
    }

    void set_ccr(u8 value) noexcept {
        x = (value & SR_X) != 0;
        n = (value & SR_N) != 0;
        z = (value & SR_Z) != 0;
        v = (value & SR_V) != 0;
        c = (value & SR_C) != 0;
    }

    [[nodiscard]] u16 sr() const noexcept {
        return static_cast<u16>(ccr() | (static_cast<u16>(int_mask) << 8) |
                                (s ? SR_S : 0) | (t1 ? SR_T1 : 0));
    }

    // Set SR and swap stack pointers if the supervisor bit changed.
    void set_sr(u16 value) noexcept {
        const bool new_s = (value & SR_S) != 0;
        if (new_s != s) {
            if (s) {
                ssp = a[7];
                a[7] = usp;
            } else {
                usp = a[7];
                a[7] = ssp;
            }
            s = new_s;
        }
        set_ccr(static_cast<u8>(value & SR_CCR_MASK));
        int_mask = static_cast<u8>((value & SR_INT_MASK) >> 8);
        t1 = (value & SR_T1) != 0;
    }

    // The stack pointer not currently in a[7].
    [[nodiscard]] u32 inactive_sp() const noexcept { return s ? usp : ssp; }
};

// 68000 condition codes, as encoded in bits 11:8 of Bcc/Scc/DBcc.
enum class Cond : u8 {
    T = 0, F = 1, HI = 2, LS = 3, CC = 4, CS = 5, NE = 6, EQ = 7,
    VC = 8, VS = 9, PL = 10, MI = 11, GE = 12, LT = 13, GT = 14, LE = 15,
};

[[nodiscard]] inline bool test_cond(const M68kState& c, Cond cc) noexcept {
    switch (cc) {
    case Cond::T:  return true;
    case Cond::F:  return false;
    case Cond::HI: return !c.c && !c.z;
    case Cond::LS: return c.c || c.z;
    case Cond::CC: return !c.c;
    case Cond::CS: return c.c;
    case Cond::NE: return !c.z;
    case Cond::EQ: return c.z;
    case Cond::VC: return !c.v;
    case Cond::VS: return c.v;
    case Cond::PL: return !c.n;
    case Cond::MI: return c.n;
    case Cond::GE: return c.n == c.v;
    case Cond::LT: return c.n != c.v;
    case Cond::GT: return (c.n == c.v) && !c.z;
    case Cond::LE: return c.z || (c.n != c.v);
    }
    return false;
}

inline const char* cond_name(Cond cc) noexcept {
    static const char* names[16] = {"t",  "f",  "hi", "ls", "cc", "cs",
                                    "ne", "eq", "vc", "vs", "pl", "mi",
                                    "ge", "lt", "gt", "le"};
    return names[static_cast<u8>(cc) & 15];
}

} // namespace arecomp
