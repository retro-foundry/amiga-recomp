// Native hooks and function replacement (AMIGA_RECOMP.md 35, 36).
//
// A hook is attached to a guest address. The recompiler forces a basic-block
// boundary there, so the dispatcher can run native code before, after, or
// instead of the guest block. This is the mechanism every enhancement is built
// on, including widescreen (docs/widescreen.md).
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "amiga_recomp/types.hpp"

namespace arecomp {

class Runtime;
struct M68kState;

enum class HookMode : u8 {
    Before,   // run native code, then the original guest block
    After,    // run the original guest block, then native code
    Replace,  // run native code instead; it supplies the next guest PC
};

// A Replace hook returns the guest address to continue at. Before/After hooks
// return value is ignored.
using HookFn = std::function<u32(M68kState&, Runtime&)>;

struct Hook {
    GuestAddr address = 0;
    HookMode mode = HookMode::Before;
    std::string name;
    HookFn fn;
    bool enabled = true;
    u64 hits = 0;
};

class HookRegistry {
public:
    void add(GuestAddr address, HookMode mode, std::string name, HookFn fn);
    void set_enabled(const std::string& name, bool enabled);

    [[nodiscard]] bool empty() const noexcept { return by_address_.empty(); }

    // Null when nothing is attached at this address, which is the common case.
    [[nodiscard]] const std::vector<Hook*>* at(GuestAddr address) const noexcept {
        if (by_address_.empty()) return nullptr;
        auto it = by_address_.find(address);
        return it == by_address_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const std::vector<Hook>& all() const noexcept { return hooks_; }
    [[nodiscard]] std::vector<Hook>& all() noexcept { return hooks_; }

private:
    void reindex();

    std::vector<Hook> hooks_;
    std::unordered_map<GuestAddr, std::vector<Hook*>> by_address_;
};

} // namespace arecomp
