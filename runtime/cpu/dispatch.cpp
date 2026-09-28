#include "amiga_recomp/dispatch.hpp"

#include <cstring>

namespace arecomp {

void BlockTable::grow(std::size_t needed) {
    std::size_t capacity = 16;
    while (capacity < needed * 2) capacity <<= 1;

    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(capacity, Slot{});
    mask_ = capacity - 1;
    count_ = 0;

    for (const Slot& s : old) {
        if (s.fn) add(s.address, s.fn);
    }
}

void BlockTable::add(GuestAddr address, BlockFn fn) {
    if (slots_.empty() || (count_ + 1) * 2 > slots_.size()) grow(count_ + 1);
    std::size_t i = hash(address) & mask_;
    for (;;) {
        Slot& s = slots_[i];
        if (s.fn == nullptr) {
            s.address = address;
            s.fn = fn;
            ++count_;
            return;
        }
        if (s.address == address) {
            s.fn = fn;   // a later definition replaces an earlier one
            return;
        }
        i = (i + 1) & mask_;
    }
}

void BlockTable::build(const BlockEntry* entries, std::size_t count) {
    slots_.clear();
    mask_ = 0;
    count_ = 0;
    grow(count ? count : 1);
    for (std::size_t i = 0; i < count; ++i) add(entries[i].address, entries[i].fn);
}

void BlockTable::invalidate(GuestAddr address) noexcept {
    if (slots_.empty()) return;
    std::size_t i = hash(address) & mask_;
    for (;;) {
        Slot& s = slots_[i];
        if (s.fn == nullptr) return;
        if (s.address == address) {
            if (s.valid) {
                s.valid = false;
                ++invalidated_;
            }
            return;
        }
        i = (i + 1) & mask_;
    }
}

void BlockTable::for_each(void (*fn)(void*, GuestAddr), void* context) const {
    for (const Slot& s : slots_)
        if (s.fn != nullptr) fn(context, s.address);
}

} // namespace arecomp
