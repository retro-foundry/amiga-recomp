// Guest-address to compiled-block dispatch (AMIGA_RECOMP.md 30).
#pragma once

#include <vector>

#include "amiga_recomp/types.hpp"

namespace arecomp {

class Runtime;
struct M68kState;

// The ABI every generated block conforms to: run the block, return the guest
// address execution continues at.
using BlockFn = u32 (*)(M68kState&, Runtime&);

struct BlockEntry {
    GuestAddr address;
    BlockFn fn;
    // Bytes of guest code this block was translated from. Lets a write be
    // matched against the exact range rather than the whole page, so a store
    // to a variable next to some code does not retire the code.
    u32 length;
};

// Open-addressed guest-address -> block lookup. Built once at start-up from
// the table the recompiler emits, then read-only, so lookups are a mask, a
// compare and (almost always) no probe.
class BlockTable {
public:
    void build(const BlockEntry* entries, std::size_t count);
    void add(GuestAddr address, BlockFn fn, u32 length = 0);

    [[nodiscard]] BlockFn find(GuestAddr address) const noexcept {
        if (slots_.empty()) return nullptr;
        std::size_t i = hash(address) & mask_;
        for (;;) {
            const Slot& s = slots_[i];
            if (s.fn == nullptr) return nullptr;
            // An invalidated slot keeps its place so the probe chain stays
            // intact; it simply stops answering.
            if (s.address == address) return s.valid ? s.fn : nullptr;
            i = (i + 1) & mask_;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    // Stop answering for this address. Used when the guest overwrites the code
    // a block was translated from (AMIGA_RECOMP.md 32).
    void invalidate(GuestAddr address) noexcept;
    [[nodiscard]] std::size_t invalidated() const noexcept { return invalidated_; }

    // Every block the table holds, for building an index by page.
    void for_each(void (*fn)(void*, GuestAddr, u32), void* context) const;

private:
    struct Slot {
        GuestAddr address = 0;
        BlockFn fn = nullptr;
        u32 length = 0;
        bool valid = true;
    };

    static std::size_t hash(GuestAddr a) noexcept {
        // Guest addresses are dense and 2-byte aligned; mix so the low zero
        // bits do not collapse the table.
        u32 h = a >> 1;
        h ^= h >> 15;
        h *= 0x2c1b3c6dU;
        h ^= h >> 12;
        return h;
    }

    void grow(std::size_t needed);

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t count_ = 0;
    std::size_t invalidated_ = 0;
};

} // namespace arecomp
