#include "amiga_recomp/guest_memory.hpp"

#include <cassert>
#include <stdexcept>

namespace arecomp {

const char* region_kind_name(RegionKind k) noexcept {
    switch (k) {
    case RegionKind::Chip: return "chip";
    case RegionKind::Fast: return "fast";
    case RegionKind::Slow: return "slow";
    case RegionKind::Rom: return "rom";
    case RegionKind::Custom: return "custom";
    case RegionKind::Cia: return "cia";
    case RegionKind::Expansion: return "expansion";
    case RegionKind::Unmapped: return "unmapped";
    }
    return "?";
}

u32 MmioDevice::read32(u32 addr) {
    const u32 hi = read16(addr);
    const u32 lo = read16(addr + 2);
    return (hi << 16) | lo;
}

void MmioDevice::write32(u32 addr, u32 value) {
    write16(addr, static_cast<u16>(value >> 16));
    write16(addr + 2, static_cast<u16>(value));
}

GuestMemory::GuestMemory(u32 address_mask) : address_mask_(address_mask) {
    pages_.resize((static_cast<std::size_t>(address_mask_) >> kPageBits) + 1);
    code_page_.assign(pages_.size(), 0);
}

void GuestMemory::add_ram(u32 start, u32 size, RegionKind kind, u8 perms,
                          const std::string& name) {
    if ((start & kPageMask) != 0 || (size & kPageMask) != 0 || size == 0)
        throw std::invalid_argument("RAM region must be non-empty and page aligned");

    auto buffer = std::make_unique<std::vector<u8>>(size, 0);
    u8* base = buffer->data();
    storage_.push_back(std::move(buffer));

    for (u32 off = 0; off < size; off += kPageSize) {
        Page& p = page_of(start + off);
        p.host = base + off;
        p.device = nullptr;
        p.perms = perms;
        p.kind = kind;
    }
    regions_.push_back({start, size, kind, perms, false, name});
}

void GuestMemory::add_rom(u32 start, const std::vector<u8>& contents,
                          const std::string& name) {
    const u32 size = (static_cast<u32>(contents.size()) + kPageMask) & ~kPageMask;
    add_ram(start, size, RegionKind::Rom, PERM_R | PERM_X, name);
    // Write through the backing store directly: the region is read-only to the
    // guest but we are populating it.
    std::memcpy(host_ptr(start), contents.data(), contents.size());
    regions_.back().kind = RegionKind::Rom;
}

void GuestMemory::map_device(u32 start, u32 size, MmioDevice* device,
                             RegionKind kind, const std::string& name) {
    if ((start & kPageMask) != 0 || (size & kPageMask) != 0 || size == 0)
        throw std::invalid_argument("device region must be non-empty and page aligned");
    for (u32 off = 0; off < size; off += kPageSize) {
        Page& p = page_of(start + off);
        p.host = nullptr;
        p.device = device;
        p.perms = PERM_RW;
        p.kind = kind;
    }
    regions_.push_back({start, size, kind, PERM_RW, true, name});
}

bool GuestMemory::load(u32 addr, const u8* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        u8* p = host_ptr(static_cast<u32>(addr + i));
        if (!p) return false;
        *p = data[i];
    }
    return true;
}

bool GuestMemory::is_mapped(u32 addr) const noexcept {
    const Page& p = page_of(addr);
    return p.host != nullptr || p.device != nullptr;
}

bool GuestMemory::is_ram(u32 addr) const noexcept {
    const Page& p = page_of(addr);
    return p.host != nullptr && (p.perms & PERM_W) != 0;
}

RegionKind GuestMemory::kind_of(u32 addr) const noexcept { return page_of(addr).kind; }

u8* GuestMemory::host_ptr(u32 addr) noexcept {
    const u32 a = addr & address_mask_;
    Page& p = page_of(a);
    return p.host ? p.host + (a & kPageMask) : nullptr;
}

const u8* GuestMemory::host_ptr(u32 addr) const noexcept {
    const u32 a = addr & address_mask_;
    const Page& p = page_of(a);
    return p.host ? p.host + (a & kPageMask) : nullptr;
}

void GuestMemory::fault(Vector v, u32 addr, bool read, bool instruction) const {
    GuestFault f;
    f.vector = v;
    f.address = addr;
    f.read = read;
    f.instruction = instruction;
    throw f;
}

u8 GuestMemory::read8(u32 addr) {
    const u32 a = addr & address_mask_;
    const Page& p = page_of(a);
    if (p.host) return p.host[a & kPageMask];
    if (p.device) return p.device->read8(a);
    if (strict_unmapped_) fault(Vector::BusError, a, true, false);
    return 0xff;
}

u16 GuestMemory::read16(u32 addr) {
    const u32 a = addr & address_mask_;
    if (a & 1) fault(Vector::AddressError, a, true, false);
    const Page& p = page_of(a);
    if (p.host) return be16(p.host + (a & kPageMask));
    if (p.device) return p.device->read16(a);
    if (strict_unmapped_) fault(Vector::BusError, a, true, false);
    return 0xffff;
}

u16 GuestMemory::fetch16(u32 addr) {
    const u32 a = addr & address_mask_;
    if (a & 1) fault(Vector::AddressError, a, true, true);
    const Page& p = page_of(a);
    if (p.host) return be16(p.host + (a & kPageMask));
    if (p.device) return p.device->read16(a);
    if (strict_unmapped_) fault(Vector::BusError, a, true, true);
    return 0xffff;
}

u32 GuestMemory::read32(u32 addr) {
    const u32 a = addr & address_mask_;
    if (a & 1) fault(Vector::AddressError, a, true, false);
    const Page& p = page_of(a);
    // A long access that stays inside one page and one RAM region is the
    // common case; otherwise fall back to two word cycles, which is also what
    // the hardware does.
    if (p.host && (a & kPageMask) <= kPageSize - 4) return be32(p.host + (a & kPageMask));
    const u32 hi = read16(a);
    const u32 lo = read16(a + 2);
    return (hi << 16) | lo;
}

void GuestMemory::write8(u32 addr, u8 value) {
    const u32 a = addr & address_mask_;
    Page& p = page_of(a);
    if (p.host) {
        if (!(p.perms & PERM_W)) return;   // ROM write: ignored, as on hardware
        p.host[a & kPageMask] = value;
        note_code_write(a);
        return;
    }
    if (p.device) {
        p.device->write8(a, value);
        return;
    }
    if (strict_unmapped_) fault(Vector::BusError, a, false, false);
}

void GuestMemory::write16(u32 addr, u16 value) {
    const u32 a = addr & address_mask_;
    if (a & 1) fault(Vector::AddressError, a, false, false);
    Page& p = page_of(a);
    if (p.host) {
        if (!(p.perms & PERM_W)) return;
        put_be16(p.host + (a & kPageMask), value);
        note_code_write(a);
        return;
    }
    if (p.device) {
        p.device->write16(a, value);
        return;
    }
    if (strict_unmapped_) fault(Vector::BusError, a, false, false);
}

void GuestMemory::write32(u32 addr, u32 value) {
    const u32 a = addr & address_mask_;
    if (a & 1) fault(Vector::AddressError, a, false, false);
    Page& p = page_of(a);
    if (p.host && (p.perms & PERM_W) && (a & kPageMask) <= kPageSize - 4) {
        put_be32(p.host + (a & kPageMask), value);
        note_code_write(a);
        return;
    }
    write16(a, static_cast<u16>(value >> 16));
    write16(a + 2, static_cast<u16>(value));
}

u8 GuestMemory::peek8(u32 addr) const noexcept {
    const u8* p = host_ptr(addr);
    return p ? *p : 0xff;
}

u16 GuestMemory::peek16(u32 addr) const noexcept {
    return static_cast<u16>((static_cast<u16>(peek8(addr)) << 8) | peek8(addr + 1));
}

u32 GuestMemory::peek32(u32 addr) const noexcept {
    return (static_cast<u32>(peek16(addr)) << 16) | peek16(addr + 2);
}

void GuestMemory::poke8(u32 addr, u8 value) noexcept {
    if (u8* p = host_ptr(addr)) *p = value;
}

void GuestMemory::poke16(u32 addr, u16 value) noexcept {
    poke8(addr, static_cast<u8>(value >> 8));
    poke8(addr + 1, static_cast<u8>(value));
}

} // namespace arecomp
