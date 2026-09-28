#include "image.hpp"

#include <cstdio>

#include "amiga_recomp/guest_memory.hpp"

namespace arecomp {

namespace {

// AmigaDOS hunk block identifiers.
enum : u32 {
    HUNK_UNIT = 0x3e7,
    HUNK_NAME = 0x3e8,
    HUNK_CODE = 0x3e9,
    HUNK_DATA = 0x3ea,
    HUNK_BSS = 0x3eb,
    HUNK_RELOC32 = 0x3ec,
    HUNK_RELOC16 = 0x3ed,
    HUNK_RELOC8 = 0x3ee,
    HUNK_EXT = 0x3ef,
    HUNK_SYMBOL = 0x3f0,
    HUNK_DEBUG = 0x3f1,
    HUNK_END = 0x3f2,
    HUNK_HEADER = 0x3f3,
};

// A cursor over a big-endian file, which refuses to read past the end instead
// of returning zeros.
class Reader {
public:
    Reader(const std::vector<u8>& data) : data_(data) {}

    bool u32_at(u32& out) {
        if (offset_ + 4 > data_.size()) return false;
        out = be32(data_.data() + offset_);
        offset_ += 4;
        return true;
    }
    bool skip(std::size_t bytes) {
        if (offset_ + bytes > data_.size()) return false;
        offset_ += bytes;
        return true;
    }
    bool copy(std::size_t bytes, std::vector<u8>& out) {
        if (offset_ + bytes > data_.size()) return false;
        out.assign(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                   data_.begin() + static_cast<std::ptrdiff_t>(offset_ + bytes));
        offset_ += bytes;
        return true;
    }
    [[nodiscard]] bool at_end() const { return offset_ >= data_.size(); }
    [[nodiscard]] std::size_t offset() const { return offset_; }

private:
    const std::vector<u8>& data_;
    std::size_t offset_ = 0;
};

} // namespace

bool read_file(const std::string& path, std::vector<u8>& out, std::string& error) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    out.clear();
    u8 buffer[65536];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0)
        out.insert(out.end(), buffer, buffer + n);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    if (!ok) error = "read error on " + path;
    return ok;
}

bool Image::contains(u32 address) const {
    for (const Segment& s : segments_) {
        if (address >= s.address && address < s.address + s.data.size()) return true;
    }
    return false;
}

bool Image::read16(u32 address, u16& out) const {
    for (const Segment& s : segments_) {
        if (address < s.address) continue;
        const u32 offset = address - s.address;
        if (offset + 1 >= s.data.size()) continue;
        out = static_cast<u16>((static_cast<u16>(s.data[offset]) << 8) |
                               s.data[offset + 1]);
        return true;
    }
    return false;
}

u16 Image::fetch(void* context, u32 address) {
    const auto* image = static_cast<const Image*>(context);
    u16 value = 0xffff;
    image->read16(address, value);
    return value;
}

bool Image::load(const Manifest& manifest, Image& out, std::string& error) {
    switch (manifest.input_mode) {
    case InputMode::Flat:
    case InputMode::Snapshot:
        return load_flat(manifest, out, error);
    case InputMode::Hunk:
        return load_hunk(manifest, out, error);
    case InputMode::Adf:
        return load_adf(manifest, out, error);
    }
    error = "unknown input mode";
    return false;
}

bool Image::load_flat(const Manifest& manifest, Image& out, std::string& error) {
    std::vector<u8> data;
    if (!read_file(manifest.resolve(manifest.input_path), data, error)) return false;

    Segment segment;
    // A snapshot is a whole memory image and therefore starts at address zero
    // unless told otherwise; a flat binary is loaded where the manifest says.
    segment.address = manifest.load_address;
    segment.executable = true;
    segment.name = manifest.input_mode == InputMode::Snapshot ? "snapshot" : "image";
    segment.data = std::move(data);
    out.add_segment(std::move(segment));
    out.set_entry(manifest.cpu.entry);
    return true;
}

bool Image::load_adf(const Manifest& manifest, Image& out, std::string& error) {
    std::vector<u8> disk;
    if (!read_file(manifest.resolve(manifest.input_path), disk, error)) return false;

    // A standard double-density Amiga disk is 80 cylinders, two heads, eleven
    // 512-byte sectors. Anything else is worth mentioning rather than assuming.
    constexpr std::size_t kStandardAdf = 80u * 2u * 11u * 512u;
    if (disk.size() != kStandardAdf) {
        std::fprintf(stderr,
                     "arecomp: note: %s is %zu bytes, not the usual %zu of a "
                     "double-density disk\n",
                     manifest.input_path.c_str(), disk.size(), kStandardAdf);
    }

    if (manifest.adf_loads.empty()) {
        error =
            "ADF input needs [[adf.load]] entries saying what the loader reads "
            "into memory. Run `arecomp bootblock <adf>` to read them off the "
            "boot block, or capture a snapshot instead (docs/decrunching.md)";
        return false;
    }

    for (const AdfLoadSpec& load : manifest.adf_loads) {
        if (static_cast<std::size_t>(load.offset) + load.length > disk.size()) {
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "[[adf.load]] at $%08x length $%08x runs past the end "
                          "of the disk image",
                          load.offset, load.length);
            error = buf;
            return false;
        }
        Segment segment;
        segment.address = load.address;
        segment.executable = load.executable;
        segment.name = "adf";
        segment.data.assign(disk.begin() + static_cast<std::ptrdiff_t>(load.offset),
                            disk.begin() + static_cast<std::ptrdiff_t>(load.offset + load.length));
        out.add_segment(std::move(segment));
    }

    out.set_entry(manifest.cpu.entry);
    return true;
}

bool Image::load_hunk(const Manifest& manifest, Image& out, std::string& error) {
    std::vector<u8> file;
    if (!read_file(manifest.resolve(manifest.input_path), file, error)) return false;

    Reader reader(file);
    u32 magic = 0;
    if (!reader.u32_at(magic) || magic != HUNK_HEADER) {
        error = "not a hunk executable (no HUNK_HEADER)";
        return false;
    }

    // Resident library names, terminated by a zero length.
    for (;;) {
        u32 length = 0;
        if (!reader.u32_at(length)) {
            error = "truncated hunk header";
            return false;
        }
        if (length == 0) break;
        if (!reader.skip(length * 4)) {
            error = "truncated resident library name";
            return false;
        }
    }

    u32 table_size = 0, first = 0, last = 0;
    if (!reader.u32_at(table_size) || !reader.u32_at(first) || !reader.u32_at(last)) {
        error = "truncated hunk header";
        return false;
    }
    if (last < first || table_size == 0) {
        error = "malformed hunk header";
        return false;
    }

    const u32 count = last - first + 1;
    std::vector<u32> sizes(count);
    for (u32 i = 0; i < count; ++i) {
        u32 size = 0;
        if (!reader.u32_at(size)) {
            error = "truncated hunk size table";
            return false;
        }
        sizes[i] = (size & 0x3fffffffu) * 4;   // top two bits are memory flags
    }

    // Lay the hunks out contiguously from the configured base. These are the
    // guest runtime addresses; everything downstream uses them.
    const u32 base = manifest.load_address ? manifest.load_address : 0x00010000;
    std::vector<u32> hunk_address(count);
    u32 cursor = base;
    for (u32 i = 0; i < count; ++i) {
        hunk_address[i] = cursor;
        cursor += (sizes[i] + 3) & ~3u;
    }

    out.segments_.resize(count);
    for (u32 i = 0; i < count; ++i) {
        out.segments_[i].address = hunk_address[i];
        out.segments_[i].data.assign(sizes[i], 0);
    }

    u32 index = 0;
    while (!reader.at_end() && index < count) {
        u32 type = 0;
        if (!reader.u32_at(type)) break;
        type &= 0x3fffffffu;

        switch (type) {
        case HUNK_CODE:
        case HUNK_DATA: {
            u32 longs = 0;
            if (!reader.u32_at(longs)) { error = "truncated hunk body"; return false; }
            std::vector<u8> body;
            if (!reader.copy(longs * 4, body)) { error = "truncated hunk body"; return false; }
            Segment& segment = out.segments_[index];
            if (body.size() > segment.data.size()) segment.data.resize(body.size());
            std::copy(body.begin(), body.end(), segment.data.begin());
            segment.executable = type == HUNK_CODE;
            segment.name = type == HUNK_CODE ? "code" : "data";
            break;
        }
        case HUNK_BSS: {
            u32 longs = 0;
            if (!reader.u32_at(longs)) { error = "truncated bss hunk"; return false; }
            Segment& segment = out.segments_[index];
            segment.bss = true;
            segment.name = "bss";
            if (segment.data.size() < longs * 4) segment.data.assign(longs * 4, 0);
            break;
        }
        case HUNK_RELOC32: {
            for (;;) {
                u32 n = 0;
                if (!reader.u32_at(n)) { error = "truncated reloc32"; return false; }
                if (n == 0) break;
                u32 target = 0;
                if (!reader.u32_at(target) || target >= count) {
                    error = "reloc32 references a hunk that does not exist";
                    return false;
                }
                for (u32 i = 0; i < n; ++i) {
                    u32 offset = 0;
                    if (!reader.u32_at(offset)) { error = "truncated reloc32"; return false; }
                    Segment& segment = out.segments_[index];
                    if (offset + 4 > segment.data.size()) {
                        error = "reloc32 offset outside its hunk";
                        return false;
                    }
                    // Relocations are applied exactly: the stored value is an
                    // offset into the target hunk, which becomes a real guest
                    // address.
                    const u32 value = be32(segment.data.data() + offset);
                    put_be32(segment.data.data() + offset, value + hunk_address[target]);
                }
            }
            break;
        }
        case HUNK_SYMBOL: {
            for (;;) {
                u32 name_longs = 0;
                if (!reader.u32_at(name_longs)) { error = "truncated symbol table"; return false; }
                if (name_longs == 0) break;
                std::vector<u8> name;
                if (!reader.copy(name_longs * 4, name)) { error = "truncated symbol"; return false; }
                u32 value = 0;
                if (!reader.u32_at(value)) { error = "truncated symbol"; return false; }
                std::string text(reinterpret_cast<const char*>(name.data()), name.size());
                while (!text.empty() && text.back() == '\0') text.pop_back();
                out.symbols_.push_back({hunk_address[index] + value, text});
            }
            break;
        }
        case HUNK_NAME:
        case HUNK_DEBUG: {
            u32 longs = 0;
            if (!reader.u32_at(longs) || !reader.skip(longs * 4)) {
                error = "truncated hunk block";
                return false;
            }
            break;
        }
        case HUNK_RELOC16:
        case HUNK_RELOC8:
            error = "16- and 8-bit relocations are not supported; they cannot be "
                    "applied to a fixed guest layout without relocating code";
            return false;
        case HUNK_END:
            ++index;
            break;
        default:
            error = "unsupported hunk type " + std::to_string(type);
            return false;
        }
    }

    out.set_entry(manifest.cpu.entry ? manifest.cpu.entry : base);
    return true;
}

} // namespace arecomp
