// Fundamental integer and guest-address types.
#pragma once

#include <cstdint>

namespace arecomp {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// A guest (68000) address. Always a guest address, never a host pointer.
using GuestAddr = u32;

// One monotonically increasing machine time base for all asynchronous
// hardware (AMIGA_RECOMP.md 17).
using MasterTick = u64;

// Operand size of a 68000 instruction.
enum class Size : u8 { Byte = 1, Word = 2, Long = 4 };

constexpr u32 size_bytes(Size s) noexcept { return static_cast<u32>(s); }

constexpr u32 size_mask(Size s) noexcept {
    switch (s) {
    case Size::Byte: return 0x000000ffu;
    case Size::Word: return 0x0000ffffu;
    default: return 0xffffffffu;
    }
}

// Sign bit position for a size.
constexpr u32 size_msb(Size s) noexcept {
    switch (s) {
    case Size::Byte: return 0x80u;
    case Size::Word: return 0x8000u;
    default: return 0x80000000u;
    }
}

constexpr u8 lo8(u32 v) noexcept { return static_cast<u8>(v); }
constexpr u16 lo16(u32 v) noexcept { return static_cast<u16>(v); }

constexpr u32 replace_lo8(u32 reg, u8 v) noexcept {
    return (reg & 0xffffff00u) | v;
}
constexpr u32 replace_lo16(u32 reg, u16 v) noexcept {
    return (reg & 0xffff0000u) | v;
}

constexpr u32 sign_extend_8(u8 v) noexcept {
    return static_cast<u32>(static_cast<s32>(static_cast<s8>(v)));
}
constexpr u32 sign_extend_16(u16 v) noexcept {
    return static_cast<u32>(static_cast<s32>(static_cast<s16>(v)));
}

// Sign-extend an arbitrary-width field held in the low bits of v.
constexpr u32 sign_extend(u32 v, unsigned bits) noexcept {
    const u32 m = 1u << (bits - 1);
    return (v ^ m) - m;
}

} // namespace arecomp
