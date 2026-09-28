// Input images: flat binaries, captured snapshots, and AmigaDOS hunk
// executables (AMIGA_RECOMP.md 4).
//
// The three concepts the spec warns about -- file offset, hunk offset and
// guest runtime address -- are kept apart here. Everything the rest of the
// tool sees is a guest address.
#pragma once

#include <string>
#include <vector>

#include "amiga_recomp/types.hpp"
#include "manifest.hpp"

namespace arecomp {

struct Segment {
    u32 address = 0;
    std::vector<u8> data;
    bool executable = false;
    bool bss = false;           // occupies space but carries no file content
    std::string name;
};

struct Symbol {
    u32 address = 0;
    std::string name;
};

class Image {
public:
    // Loads according to the manifest's input mode.
    static bool load(const Manifest& manifest, Image& out, std::string& error);

    [[nodiscard]] const std::vector<Segment>& segments() const { return segments_; }
    [[nodiscard]] const std::vector<Symbol>& symbols() const { return symbols_; }
    [[nodiscard]] u32 entry() const { return entry_; }

    // Big-endian word at a guest address. Returns false if unmapped, which is
    // how discovery knows it has walked off the end of the code.
    bool read16(u32 address, u16& out) const;
    [[nodiscard]] bool contains(u32 address) const;

    // Fetch helper matching the decoder's callback signature. Out-of-range
    // reads return 0xffff, which decodes to an illegal instruction and stops
    // discovery rather than inventing code.
    static u16 fetch(void* context, u32 address);

    void add_segment(Segment segment) { segments_.push_back(std::move(segment)); }
    void set_entry(u32 entry) { entry_ = entry; }

private:
    static bool load_flat(const Manifest& manifest, Image& out, std::string& error);
    static bool load_hunk(const Manifest& manifest, Image& out, std::string& error);
    static bool load_adf(const Manifest& manifest, Image& out, std::string& error);

    std::vector<Segment> segments_;
    std::vector<Symbol> symbols_;
    u32 entry_ = 0;
};

// Reads a whole file. Reports failure rather than returning an empty vector
// that looks like a zero-length file.
bool read_file(const std::string& path, std::vector<u8>& out, std::string& error);

} // namespace arecomp
