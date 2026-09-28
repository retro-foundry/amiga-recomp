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
};

// Open-addressed guest-address -> block lookup. Built once at start-up from
// the table the recompiler emits, then read-only, so lookups are a mask, a
// compare and (almost always) no probe.
class BlockTable {
public:
    void build(const BlockEntry* entries, std::size_t count);
    void add(GuestAddr address, BlockFn fn);

    [[nodiscard]] BlockFn find(GuestAddr address) const noexcept {
        if (slots_.empty()) return nullptr;
        std::size_t i = hash(address) & mask_;
        for (;;) {
            const Slot& s = slots_[i];
            if (s.fn == nullptr) return nullptr;
            if (s.address == address) return s.fn;
            i = (i + 1) & mask_;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    struct Slot {
        GuestAddr address = 0;
        BlockFn fn = nullptr;
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
};

} // namespace arecomp
