// Guest address space: region routing, big-endian access, MMIO dispatch
// (AMIGA_RECOMP.md 7).
//
// Guest memory is big-endian regardless of host endianness, and no host
// pointer is ever handed to translated code. Plain RAM still gets a fast path:
// a page table resolves an address to a host backing pointer in one lookup,
// and the big-endian assembly happens explicitly.
#pragma once

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "amiga_recomp/exceptions.hpp"
#include "amiga_recomp/types.hpp"

namespace arecomp {

enum class RegionKind : u8 {
    Chip,
    Fast,
    Slow,       // "ranger" RAM at $C00000
    Rom,
    Custom,     // $DFF000 custom chip registers
    Cia,        // $BFxxxx
    Expansion,
    Unmapped,
};

const char* region_kind_name(RegionKind k) noexcept;

enum : u8 {
    PERM_R = 1,
    PERM_W = 2,
    PERM_X = 4,
    PERM_RW = PERM_R | PERM_W,
    PERM_RWX = PERM_R | PERM_W | PERM_X,
};

// A device occupying part of the address space whose reads and writes are not
// memory: custom chip registers, CIAs. Accesses have side effects and must
// never be modelled as an array (AMIGA_RECOMP.md 18, rule 12).
class MmioDevice {
public:
    virtual ~MmioDevice() = default;
    virtual u8 read8(u32 addr) = 0;
    virtual u16 read16(u32 addr) = 0;
    virtual void write8(u32 addr, u8 value) = 0;
    virtual void write16(u32 addr, u16 value) = 0;
    // 32-bit accesses to custom registers are two independent 16-bit cycles on
    // real hardware, which matters for registers with side effects.
    virtual u32 read32(u32 addr);
    virtual void write32(u32 addr, u32 value);
};

class GuestMemory {
public:
    // 4 KiB pages over the 24-bit classic Amiga address space.
    static constexpr u32 kPageBits = 12;
    static constexpr u32 kPageSize = 1u << kPageBits;
    static constexpr u32 kPageMask = kPageSize - 1;

    explicit GuestMemory(u32 address_mask = 0x00ffffffu);

    [[nodiscard]] u32 address_mask() const noexcept { return address_mask_; }
    void set_address_mask(u32 mask) noexcept { address_mask_ = mask; }

    // Add a RAM or ROM region. start and size must be page aligned.
    void add_ram(u32 start, u32 size, RegionKind kind, u8 perms = PERM_RWX,
                 const std::string& name = {});
    void add_rom(u32 start, const std::vector<u8>& contents,
                 const std::string& name = {});

    // Route a range to a device. start and size must be page aligned.
    void map_device(u32 start, u32 size, MmioDevice* device, RegionKind kind,
                    const std::string& name = {});

    // Copy an image into already-mapped RAM. Returns false if the range is not
    // fully backed by RAM.
    bool load(u32 addr, const u8* data, std::size_t size);

    [[nodiscard]] bool is_mapped(u32 addr) const noexcept;
    [[nodiscard]] bool is_ram(u32 addr) const noexcept;
    [[nodiscard]] RegionKind kind_of(u32 addr) const noexcept;

    // Raw host pointer to backing RAM, for the runtime's own use (DMA, the
    // display renderer, debug tooling). Never exposed to translated code.
    [[nodiscard]] u8* host_ptr(u32 addr) noexcept;
    [[nodiscard]] const u8* host_ptr(u32 addr) const noexcept;

    // --- guest accessors -------------------------------------------------
    // Word and long accesses to odd addresses raise an address error rather
    // than being silently fixed up (rule 10).

    u8 read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);
    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    // Instruction fetch: same as read16 but reports the fault as an
    // instruction access, which the exception frame records.
    u16 fetch16(u32 addr);

    // Accessors that never fault, for debuggers and tests.
    [[nodiscard]] u8 peek8(u32 addr) const noexcept;
    [[nodiscard]] u16 peek16(u32 addr) const noexcept;
    [[nodiscard]] u32 peek32(u32 addr) const noexcept;
    void poke8(u32 addr, u8 value) noexcept;
    void poke16(u32 addr, u16 value) noexcept;

    // When true, an access to unmapped space raises a bus error. When false it
    // reads as 0xff and swallows writes, which some captured snapshots need.
    void set_strict_unmapped(bool strict) noexcept { strict_unmapped_ = strict; }
    [[nodiscard]] bool strict_unmapped() const noexcept { return strict_unmapped_; }

    struct RegionInfo {
        u32 start = 0;
        u32 size = 0;
        RegionKind kind = RegionKind::Unmapped;
        u8 perms = 0;
        bool device = false;
        std::string name;
    };
    [[nodiscard]] const std::vector<RegionInfo>& regions() const noexcept { return regions_; }

private:
    struct Page {
        u8* host = nullptr;        // backing store for plain RAM/ROM
        MmioDevice* device = nullptr;
        u8 perms = 0;
        RegionKind kind = RegionKind::Unmapped;
    };

    [[nodiscard]] const Page& page_of(u32 addr) const noexcept {
        return pages_[(addr & address_mask_) >> kPageBits];
    }
    [[nodiscard]] Page& page_of(u32 addr) noexcept {
        return pages_[(addr & address_mask_) >> kPageBits];
    }

    [[noreturn]] void fault(Vector v, u32 addr, bool read, bool instruction) const;

    u32 address_mask_;
    std::vector<Page> pages_;
    std::vector<std::unique_ptr<std::vector<u8>>> storage_;
    std::vector<RegionInfo> regions_;
    bool strict_unmapped_ = true;
};

// Big-endian helpers for raw buffers, used by loaders and the chipset.
inline u16 be16(const u8* p) noexcept {
    return static_cast<u16>((static_cast<u16>(p[0]) << 8) | p[1]);
}
inline u32 be32(const u8* p) noexcept {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}
inline void put_be16(u8* p, u16 v) noexcept {
    p[0] = static_cast<u8>(v >> 8);
    p[1] = static_cast<u8>(v);
}
inline void put_be32(u8* p, u32 v) noexcept {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

} // namespace arecomp
