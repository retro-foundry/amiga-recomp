// Saving and restoring custom chip state (AMIGA_RECOMP.md 39).
//
// Capturing a running game's memory is not enough on its own. The chips hold
// state the game set up earlier and expects to still be there: where the
// Copper list is and how far through it the Copper has got, which DMA channels
// and interrupts are enabled, where the bitplane and sprite pointers are, and
// the palette. Restore the memory without them and the game wakes up on a
// machine it never configured.

#include <cstdio>

#include "amiga_recomp/chipset.hpp"

namespace arecomp {

namespace {

constexpr u32 kMagic = 0x41524353;   // "ARCS"
constexpr u32 kVersion = 1;

void put32(std::FILE* f, u32 value) {
    const u8 bytes[4] = {static_cast<u8>(value >> 24), static_cast<u8>(value >> 16),
                         static_cast<u8>(value >> 8), static_cast<u8>(value)};
    std::fwrite(bytes, 1, 4, f);
}

bool get32(std::FILE* f, u32& value) {
    u8 bytes[4];
    if (std::fread(bytes, 1, 4, f) != 4) return false;
    value = (static_cast<u32>(bytes[0]) << 24) | (static_cast<u32>(bytes[1]) << 16) |
            (static_cast<u32>(bytes[2]) << 8) | bytes[3];
    return true;
}

} // namespace

bool Chipset::save_state(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    put32(f, kMagic);
    put32(f, kVersion);

    // The whole register file. Reads of it are synthesised, but the stored
    // values are what the game wrote and what the hardware acts on.
    for (u16 value : regs_) put32(f, value);

    // Derived state the register file does not capture.
    put32(f, copper_.pc);
    put32(f, copper_.waiting ? 1u : 0u);
    put32(f, copper_.wait_vpos);
    put32(f, copper_.wait_hpos);
    put32(f, copper_.wait_mask_v);
    put32(f, copper_.wait_mask_h);
    for (u32 p : bpl_pointer_) put32(f, p);
    for (const Sprite& s : sprites_) put32(f, s.pointer);
    put32(f, vpos_);
    put32(f, hpos_);

    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

bool Chipset::load_state(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    u32 magic = 0, version = 0;
    if (!get32(f, magic) || !get32(f, version) || magic != kMagic ||
        version != kVersion) {
        std::fclose(f);
        return false;
    }

    for (u16& value : regs_) {
        u32 stored = 0;
        if (!get32(f, stored)) { std::fclose(f); return false; }
        value = static_cast<u16>(stored);
    }

    u32 value = 0;
    if (!get32(f, copper_.pc)) { std::fclose(f); return false; }
    if (!get32(f, value)) { std::fclose(f); return false; }
    copper_.waiting = value != 0;
    u32 v = 0;
    if (!get32(f, v)) { std::fclose(f); return false; }
    copper_.wait_vpos = static_cast<u16>(v);
    if (!get32(f, v)) { std::fclose(f); return false; }
    copper_.wait_hpos = static_cast<u16>(v);
    if (!get32(f, v)) { std::fclose(f); return false; }
    copper_.wait_mask_v = static_cast<u16>(v);
    if (!get32(f, v)) { std::fclose(f); return false; }
    copper_.wait_mask_h = static_cast<u16>(v);
    for (u32& p : bpl_pointer_) if (!get32(f, p)) { std::fclose(f); return false; }
    for (Sprite& s : sprites_) if (!get32(f, s.pointer)) { std::fclose(f); return false; }
    if (!get32(f, vpos_)) { std::fclose(f); return false; }
    if (!get32(f, hpos_)) { std::fclose(f); return false; }

    std::fclose(f);

    // Bring the derived state in line with the registers just restored.
    copper_.enabled = (regs_[reg::DMACON >> 1] & (DMAF_MASTER | DMAF_COPPER)) ==
                      (DMAF_MASTER | DMAF_COPPER);
    recompute_display_window();
    update_interrupts();
    return true;
}

} // namespace arecomp
