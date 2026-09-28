// Shared MC68000 instruction semantics: ALU results and condition codes.
//
// Both the generated C++ blocks and the reference interpreter call into this
// header, so an instruction has exactly one definition of its arithmetic and
// its flags (AMIGA_RECOMP.md 13: do not duplicate flag formulas in every
// translator).
//
// Flag rules follow the MC68000 User's Manual. Where the manual says
// "undefined" (BCD N/V, DIV overflow) we reproduce the behaviour of real
// 68000 silicon as modelled by the usual reference implementations, so that
// differential testing against an external oracle is meaningful.
#pragma once

#include "amiga_recomp/cpu_state.hpp"
#include "amiga_recomp/types.hpp"

namespace arecomp::sem {

// ---------------------------------------------------------------------------
// Generic width-parameterised cores. The named wrappers at the bottom are what
// generated code calls.
// ---------------------------------------------------------------------------

template <unsigned W> struct Width;
template <> struct Width<8>  { static constexpr u32 mask = 0xffu;       static constexpr u32 msb = 0x80u; };
template <> struct Width<16> { static constexpr u32 mask = 0xffffu;     static constexpr u32 msb = 0x8000u; };
template <> struct Width<32> { static constexpr u32 mask = 0xffffffffu; static constexpr u32 msb = 0x80000000u; };

// N and Z from a result, V and C cleared. MOVE, TST, AND, OR, EOR, NOT, EXT,
// SWAP and TAS all share this.
template <unsigned W> inline void logic_flags(M68kState& c, u32 res) noexcept {
    res &= Width<W>::mask;
    c.n = (res & Width<W>::msb) != 0;
    c.z = res == 0;
    c.v = 0;
    c.c = 0;
}

template <unsigned W> inline u32 add(M68kState& c, u32 dst, u32 src) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    dst &= mask; src &= mask;
    const u32 res = (dst + src) & mask;
    const u32 carry = ((src & dst) | (~res & (src | dst))) & msb;
    const u32 ovf = ((src ^ res) & (dst ^ res)) & msb;
    c.n = (res & msb) != 0;
    c.z = res == 0;
    c.v = ovf != 0;
    c.c = c.x = carry != 0;
    return res;
}

template <unsigned W> inline u32 addx(M68kState& c, u32 dst, u32 src) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    dst &= mask; src &= mask;
    const u32 res = (dst + src + (c.x ? 1u : 0u)) & mask;
    const u32 carry = ((src & dst) | (~res & (src | dst))) & msb;
    const u32 ovf = ((src ^ res) & (dst ^ res)) & msb;
    c.n = (res & msb) != 0;
    if (res != 0) c.z = 0;   // Z accumulates: cleared if non-zero, else unchanged
    c.v = ovf != 0;
    c.c = c.x = carry != 0;
    return res;
}

template <unsigned W> inline u32 sub(M68kState& c, u32 dst, u32 src) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    dst &= mask; src &= mask;
    const u32 res = (dst - src) & mask;
    const u32 borrow = ((src & ~dst) | (res & (src | ~dst))) & msb;
    const u32 ovf = ((src ^ dst) & (res ^ dst)) & msb;
    c.n = (res & msb) != 0;
    c.z = res == 0;
    c.v = ovf != 0;
    c.c = c.x = borrow != 0;
    return res;
}

template <unsigned W> inline u32 subx(M68kState& c, u32 dst, u32 src) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    dst &= mask; src &= mask;
    const u32 res = (dst - src - (c.x ? 1u : 0u)) & mask;
    const u32 borrow = ((src & ~dst) | (res & (src | ~dst))) & msb;
    const u32 ovf = ((src ^ dst) & (res ^ dst)) & msb;
    c.n = (res & msb) != 0;
    if (res != 0) c.z = 0;
    c.v = ovf != 0;
    c.c = c.x = borrow != 0;
    return res;
}

// CMP is SUB without X and without writing back.
template <unsigned W> inline void cmp(M68kState& c, u32 dst, u32 src) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    dst &= mask; src &= mask;
    const u32 res = (dst - src) & mask;
    const u32 borrow = ((src & ~dst) | (res & (src | ~dst))) & msb;
    const u32 ovf = ((src ^ dst) & (res ^ dst)) & msb;
    c.n = (res & msb) != 0;
    c.z = res == 0;
    c.v = ovf != 0;
    c.c = borrow != 0;
}

template <unsigned W> inline u32 neg(M68kState& c, u32 dst) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    dst &= mask;
    const u32 res = (0u - dst) & mask;
    c.n = (res & msb) != 0;
    c.z = res == 0;
    c.v = (res & dst & msb) != 0;
    c.c = c.x = res != 0;
    return res;
}

template <unsigned W> inline u32 negx(M68kState& c, u32 dst) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    dst &= mask;
    const u32 res = (0u - dst - (c.x ? 1u : 0u)) & mask;
    c.n = (res & msb) != 0;
    if (res != 0) c.z = 0;
    c.v = (res & dst & msb) != 0;
    // Borrow out of 0 - dst - X.
    c.c = c.x = (dst != 0) || (res != 0);
    return res;
}

inline void clr_flags(M68kState& c) noexcept {
    c.n = 0; c.z = 1; c.v = 0; c.c = 0;
}

// ---------------------------------------------------------------------------
// Shifts and rotates. count is the executed shift count, 0..63.
// ---------------------------------------------------------------------------

template <unsigned W> inline u32 asl(M68kState& c, u32 src, unsigned count) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    src &= mask;
    if (count == 0) {
        c.v = 0; c.c = 0;
        c.n = (src & msb) != 0; c.z = src == 0;
        return src;
    }
    u32 res;
    if (count >= W) {
        res = 0;
        c.c = c.x = (count == W) ? ((src & 1u) != 0) : 0;
        c.v = src != 0;
    } else {
        res = (src << count) & mask;
        c.c = c.x = ((src >> (W - count)) & 1u) != 0;
        // V is set if the sign bit changed at any point, i.e. the top count+1
        // bits of the source were not all identical.
        const u32 top = mask & ~(mask >> (count + 1));
        const u32 bits = src & top;
        c.v = (bits != 0) && (bits != top);
    }
    c.n = (res & msb) != 0;
    c.z = res == 0;
    return res;
}

template <unsigned W> inline u32 lsl(M68kState& c, u32 src, unsigned count) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    src &= mask;
    if (count == 0) {
        c.v = 0; c.c = 0;
        c.n = (src & msb) != 0; c.z = src == 0;
        return src;
    }
    u32 res;
    if (count >= W) {
        res = 0;
        c.c = c.x = (count == W) ? ((src & 1u) != 0) : 0;
    } else {
        res = (src << count) & mask;
        c.c = c.x = ((src >> (W - count)) & 1u) != 0;
    }
    c.v = 0;
    c.n = (res & msb) != 0;
    c.z = res == 0;
    return res;
}

template <unsigned W> inline u32 lsr(M68kState& c, u32 src, unsigned count) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    src &= mask;
    if (count == 0) {
        c.v = 0; c.c = 0;
        c.n = (src & msb) != 0; c.z = src == 0;
        return src;
    }
    u32 res;
    if (count >= W) {
        res = 0;
        c.c = c.x = (count == W) ? ((src & msb) != 0) : 0;
    } else {
        res = (src >> count) & mask;
        c.c = c.x = ((src >> (count - 1)) & 1u) != 0;
    }
    c.v = 0;
    c.n = 0;          // a logical right shift always clears the MSB
    c.z = res == 0;
    return res;
}

template <unsigned W> inline u32 asr(M68kState& c, u32 src, unsigned count) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    src &= mask;
    const bool sign = (src & msb) != 0;
    if (count == 0) {
        c.v = 0; c.c = 0;
        c.n = sign; c.z = src == 0;
        return src;
    }
    u32 res;
    if (count >= W) {
        res = sign ? mask : 0u;
        c.c = c.x = sign;
    } else {
        res = (src >> count) & mask;
        if (sign) res |= (mask << (W - count)) & mask;
        c.c = c.x = ((src >> (count - 1)) & 1u) != 0;
    }
    c.v = 0;
    c.n = (res & msb) != 0;
    c.z = res == 0;
    return res;
}

template <unsigned W> inline u32 rol(M68kState& c, u32 src, unsigned count) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    src &= mask;
    c.v = 0;
    if (count == 0) {
        c.c = 0;
        c.n = (src & msb) != 0; c.z = src == 0;
        return src;
    }
    const unsigned k = count % W;
    const u32 res = k ? (((src << k) | (src >> (W - k))) & mask) : src;
    // The bit rotated out of the MSB lands in bit 0, so for any non-zero count
    // bit 0 of the result is the last bit rotated out.
    c.c = (res & 1u) != 0;
    c.n = (res & msb) != 0;
    c.z = res == 0;
    return res;
}

template <unsigned W> inline u32 ror(M68kState& c, u32 src, unsigned count) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    src &= mask;
    c.v = 0;
    if (count == 0) {
        c.c = 0;
        c.n = (src & msb) != 0; c.z = src == 0;
        return src;
    }
    const unsigned k = count % W;
    const u32 res = k ? (((src >> k) | (src << (W - k))) & mask) : src;
    c.c = (res & msb) != 0;
    c.n = (res & msb) != 0;
    c.z = res == 0;
    return res;
}

// ROXL/ROXR rotate the W+1 bit quantity {X, operand}.
template <unsigned W> inline u32 roxl(M68kState& c, u32 src, unsigned count) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    src &= mask;
    c.v = 0;
    if (count == 0) {
        c.c = c.x;                       // X is unchanged and copied into C
        c.n = (src & msb) != 0; c.z = src == 0;
        return src;
    }
    const unsigned k = count % (W + 1);
    u64 v = (static_cast<u64>(c.x ? 1u : 0u) << W) | src;
    if (k) {
        const u64 wide = (W == 32) ? 0x1ffffffffull : ((1ull << (W + 1)) - 1);
        v = ((v << k) | (v >> (W + 1 - k))) & wide;
    }
    const u32 res = static_cast<u32>(v) & mask;
    c.x = c.c = ((v >> W) & 1u) != 0;
    c.n = (res & msb) != 0;
    c.z = res == 0;
    return res;
}

template <unsigned W> inline u32 roxr(M68kState& c, u32 src, unsigned count) noexcept {
    constexpr u32 mask = Width<W>::mask, msb = Width<W>::msb;
    src &= mask;
    c.v = 0;
    if (count == 0) {
        c.c = c.x;
        c.n = (src & msb) != 0; c.z = src == 0;
        return src;
    }
    const unsigned k = count % (W + 1);
    u64 v = (static_cast<u64>(c.x ? 1u : 0u) << W) | src;
    if (k) {
        const u64 wide = (W == 32) ? 0x1ffffffffull : ((1ull << (W + 1)) - 1);
        v = ((v >> k) | (v << (W + 1 - k))) & wide;
    }
    const u32 res = static_cast<u32>(v) & mask;
    c.x = c.c = ((v >> W) & 1u) != 0;
    c.n = (res & msb) != 0;
    c.z = res == 0;
    return res;
}

// ---------------------------------------------------------------------------
// Multiply and divide
// ---------------------------------------------------------------------------

inline u32 mulu(M68kState& c, u16 dst, u16 src) noexcept {
    const u32 res = static_cast<u32>(dst) * static_cast<u32>(src);
    logic_flags<32>(c, res);
    return res;
}

inline u32 muls(M68kState& c, u16 dst, u16 src) noexcept {
    const s32 res = static_cast<s32>(static_cast<s16>(dst)) *
                    static_cast<s32>(static_cast<s16>(src));
    const u32 r = static_cast<u32>(res);
    logic_flags<32>(c, r);
    return r;
}

struct DivResult {
    u32 value = 0;             // remainder:quotient, as the 68000 writes it back
    bool overflow = false;     // quotient did not fit: V set, destination untouched
    bool divide_by_zero = false;
};

inline DivResult divu(M68kState& c, u32 dst, u16 src) noexcept {
    DivResult r;
    if (src == 0) {
        r.divide_by_zero = true;
        return r;
    }
    const u32 quotient = dst / src;
    const u32 remainder = dst % src;
    if (quotient > 0xffffu) {
        c.v = 1;               // only V is affected on overflow
        r.overflow = true;
        return r;
    }
    c.n = (quotient & 0x8000u) != 0;
    c.z = quotient == 0;
    c.v = 0;
    c.c = 0;
    r.value = (remainder << 16) | (quotient & 0xffffu);
    return r;
}

inline DivResult divs(M68kState& c, u32 dst, u16 src) noexcept {
    DivResult r;
    if (src == 0) {
        r.divide_by_zero = true;
        return r;
    }
    // -2^31 / -1 traps on the host; the 68000 produces zero with cleared flags.
    if (dst == 0x80000000u && src == 0xffffu) {
        c.n = 0; c.z = 1; c.v = 0; c.c = 0;
        r.value = 0;
        return r;
    }
    const s32 a = static_cast<s32>(dst);
    const s32 b = static_cast<s32>(static_cast<s16>(src));
    const s32 quotient = a / b;
    const s32 remainder = a % b;   // C++ truncates toward zero, as the 68000 does
    if (quotient != static_cast<s32>(static_cast<s16>(quotient))) {
        c.v = 1;
        r.overflow = true;
        return r;
    }
    c.n = (quotient & 0x8000) != 0;
    c.z = quotient == 0;
    c.v = 0;
    c.c = 0;
    r.value = (static_cast<u32>(remainder) << 16) | (static_cast<u32>(quotient) & 0xffffu);
    return r;
}

// ---------------------------------------------------------------------------
// BCD. N and V are documented as undefined; these formulas reproduce what the
// silicon actually leaves behind, which is what an oracle will report.
// ---------------------------------------------------------------------------

inline u8 abcd(M68kState& c, u8 dst, u8 src) noexcept {
    u32 res = (src & 0x0fu) + (dst & 0x0fu) + (c.x ? 1u : 0u);
    u32 vflag = ~res;
    if (res > 9) res += 6;
    res += (src & 0xf0u) + (dst & 0xf0u);
    const bool carry = res > 0x99u;
    c.c = c.x = carry;
    if (carry) res -= 0xa0u;
    vflag &= res;
    c.v = (vflag & 0x80u) != 0;
    c.n = (res & 0x80u) != 0;
    const u8 out = static_cast<u8>(res);
    if (out != 0) c.z = 0;
    return out;
}

inline u8 sbcd(M68kState& c, u8 dst, u8 src) noexcept {
    u32 res = (dst & 0x0fu) - (src & 0x0fu) - (c.x ? 1u : 0u);
    u32 vflag = ~res;
    if (res > 9) res -= 6;
    res += (dst & 0xf0u) - (src & 0xf0u);
    const bool borrow = res > 0x99u;
    c.c = c.x = borrow;
    if (borrow) res += 0xa0u;
    vflag &= res;
    c.v = (vflag & 0x80u) != 0;
    c.n = (res & 0x80u) != 0;
    const u8 out = static_cast<u8>(res);
    if (out != 0) c.z = 0;
    return out;
}

inline u8 nbcd(M68kState& c, u8 dst) noexcept {
    u32 res = 0x9au - dst - (c.x ? 1u : 0u);
    if (res != 0x9au) {
        u32 vflag = ~res;
        if ((res & 0x0fu) == 0x0au) res = (res & 0xf0u) + 0x10u;
        res &= 0xffu;
        vflag &= res;
        c.v = (vflag & 0x80u) != 0;
        if (res != 0) c.z = 0;
        c.c = c.x = 1;
    } else {
        res = 0;
        c.v = 0;
        c.c = c.x = 0;
    }
    c.n = (res & 0x80u) != 0;
    return static_cast<u8>(res);
}

// ---------------------------------------------------------------------------
// Named wrappers. Generated code reads better with the width spelled out.
// ---------------------------------------------------------------------------

#define ARECOMP_SEM_BINOP(name)                                                            \
    inline u32 name##8(M68kState& c, u32 d, u32 s) noexcept { return name<8>(c, d, s); }   \
    inline u32 name##16(M68kState& c, u32 d, u32 s) noexcept { return name<16>(c, d, s); } \
    inline u32 name##32(M68kState& c, u32 d, u32 s) noexcept { return name<32>(c, d, s); }
ARECOMP_SEM_BINOP(add)
ARECOMP_SEM_BINOP(addx)
ARECOMP_SEM_BINOP(sub)
ARECOMP_SEM_BINOP(subx)
#undef ARECOMP_SEM_BINOP

inline void cmp8(M68kState& c, u32 d, u32 s) noexcept { cmp<8>(c, d, s); }
inline void cmp16(M68kState& c, u32 d, u32 s) noexcept { cmp<16>(c, d, s); }
inline void cmp32(M68kState& c, u32 d, u32 s) noexcept { cmp<32>(c, d, s); }

inline u32 neg8(M68kState& c, u32 d) noexcept { return neg<8>(c, d); }
inline u32 neg16(M68kState& c, u32 d) noexcept { return neg<16>(c, d); }
inline u32 neg32(M68kState& c, u32 d) noexcept { return neg<32>(c, d); }
inline u32 negx8(M68kState& c, u32 d) noexcept { return negx<8>(c, d); }
inline u32 negx16(M68kState& c, u32 d) noexcept { return negx<16>(c, d); }
inline u32 negx32(M68kState& c, u32 d) noexcept { return negx<32>(c, d); }

inline void logic_flags8(M68kState& c, u32 r) noexcept { logic_flags<8>(c, r); }
inline void logic_flags16(M68kState& c, u32 r) noexcept { logic_flags<16>(c, r); }
inline void logic_flags32(M68kState& c, u32 r) noexcept { logic_flags<32>(c, r); }

#define ARECOMP_SEM_SHIFT(name)                                                                 \
    inline u32 name##8(M68kState& c, u32 v, unsigned n) noexcept { return name<8>(c, v, n); }   \
    inline u32 name##16(M68kState& c, u32 v, unsigned n) noexcept { return name<16>(c, v, n); } \
    inline u32 name##32(M68kState& c, u32 v, unsigned n) noexcept { return name<32>(c, v, n); }
ARECOMP_SEM_SHIFT(asl)
ARECOMP_SEM_SHIFT(asr)
ARECOMP_SEM_SHIFT(lsl)
ARECOMP_SEM_SHIFT(lsr)
ARECOMP_SEM_SHIFT(rol)
ARECOMP_SEM_SHIFT(ror)
ARECOMP_SEM_SHIFT(roxl)
ARECOMP_SEM_SHIFT(roxr)
#undef ARECOMP_SEM_SHIFT

// Bit operations: Z reflects the tested bit, nothing else changes.
inline void btst_flag(M68kState& c, u32 value, unsigned bit) noexcept {
    c.z = ((value >> bit) & 1u) == 0;
}

} // namespace arecomp::sem
