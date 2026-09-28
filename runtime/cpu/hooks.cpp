#include "amiga_recomp/hooks.hpp"

namespace arecomp {

void HookRegistry::reindex() {
    by_address_.clear();
    for (Hook& h : hooks_) by_address_[h.address].push_back(&h);
}

void HookRegistry::add(GuestAddr address, HookMode mode, std::string name, HookFn fn) {
    hooks_.push_back(Hook{address, mode, std::move(name), std::move(fn), true, 0});
    // Hook objects are stored by value, so any push_back may reallocate and
    // invalidate the index. Rebuild it rather than keeping stale pointers.
    reindex();
}

void HookRegistry::set_enabled(const std::string& name, bool enabled) {
    for (Hook& h : hooks_)
        if (h.name == name) h.enabled = enabled;
}

} // namespace arecomp
