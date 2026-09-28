#include "amiga_recomp/guest_memory.hpp"

#include "test_support.hpp"

using namespace arecomp;

namespace {

// A stand-in for a custom-chip register block: every access is observable, and
// nothing behaves like RAM.
class RecordingDevice : public MmioDevice {
public:
    u8 read8(u32 addr) override {
        reads.push_back({addr, 0, 1});
        return 0x5a;
    }
    u16 read16(u32 addr) override {
        reads.push_back({addr, 0, 2});
        return 0x1234;
    }
    void write8(u32 addr, u8 value) override { writes.push_back({addr, value, 1}); }
    void write16(u32 addr, u16 value) override { writes.push_back({addr, value, 2}); }

    struct Access { u32 addr; u32 value; u8 size; };
    std::vector<Access> reads;
    std::vector<Access> writes;
};

GuestMemory make_memory() {
    GuestMemory mem(0x00ffffff);
    mem.add_ram(0x000000, 0x080000, RegionKind::Chip, PERM_RWX, "chip");
    return mem;
}

} // namespace

TEST_CASE("guest memory is big-endian regardless of the host") {
    auto mem = make_memory();
    mem.write32(0x1000, 0x11223344);
    CHECK_EQ(mem.read8(0x1000), 0x11u);
    CHECK_EQ(mem.read8(0x1001), 0x22u);
    CHECK_EQ(mem.read8(0x1002), 0x33u);
    CHECK_EQ(mem.read8(0x1003), 0x44u);
    CHECK_EQ(mem.read16(0x1000), 0x1122u);
    CHECK_EQ(mem.read32(0x1000), 0x11223344u);
}

TEST_CASE("word and long accesses span page boundaries correctly") {
    auto mem = make_memory();
    const u32 boundary = GuestMemory::kPageSize - 2;
    mem.write32(boundary, 0xaabbccdd);
    CHECK_EQ(mem.read32(boundary), 0xaabbccddu);
    CHECK_EQ(mem.read16(boundary), 0xaabbu);
    CHECK_EQ(mem.read16(boundary + 2), 0xccddu);
}

TEST_CASE("an odd word access raises an address error, it is not fixed up") {
    auto mem = make_memory();
    bool faulted = false;
    try {
        (void)mem.read16(0x1001);
    } catch (const GuestFault& f) {
        faulted = true;
        CHECK(f.vector == Vector::AddressError);
        CHECK_EQ(f.address, 0x1001u);
        CHECK(f.read);
        CHECK(!f.instruction);
    }
    CHECK(faulted);
}

TEST_CASE("an odd long write raises an address error") {
    auto mem = make_memory();
    bool faulted = false;
    try {
        mem.write32(0x1001, 0);
    } catch (const GuestFault& f) {
        faulted = true;
        CHECK(!f.read);
    }
    CHECK(faulted);
}

TEST_CASE("odd byte accesses are perfectly legal") {
    auto mem = make_memory();
    mem.write8(0x1001, 0x7f);
    CHECK_EQ(mem.read8(0x1001), 0x7fu);
}

TEST_CASE("an instruction fetch records itself as one in the fault") {
    auto mem = make_memory();
    bool faulted = false;
    try {
        (void)mem.fetch16(0x1001);
    } catch (const GuestFault& f) {
        faulted = true;
        CHECK(f.instruction);
    }
    CHECK(faulted);
}

TEST_CASE("unmapped space raises a bus error when strict") {
    auto mem = make_memory();
    bool faulted = false;
    try {
        (void)mem.read8(0x400000);
    } catch (const GuestFault& f) {
        faulted = true;
        CHECK(f.vector == Vector::BusError);
    }
    CHECK(faulted);
}

TEST_CASE("unmapped space can be made permissive for captured snapshots") {
    auto mem = make_memory();
    mem.set_strict_unmapped(false);
    CHECK_EQ(mem.read8(0x400000), 0xffu);
    mem.write8(0x400000, 0x12);   // swallowed, not a fault
}

TEST_CASE("addresses are masked to the guest's address width") {
    auto mem = make_memory();
    mem.write16(0x001000, 0xbeef);
    // The 68000 has 24 address lines: bits above them do not exist.
    CHECK_EQ(mem.read16(0xff001000), 0xbeefu);
}

TEST_CASE("a rom region ignores writes instead of faulting") {
    GuestMemory mem(0x00ffffff);
    std::vector<u8> contents(GuestMemory::kPageSize, 0xa5);
    mem.add_rom(0x00f80000, contents, "kickstart-stand-in");
    CHECK_EQ(mem.read8(0x00f80000), 0xa5u);
    mem.write8(0x00f80000, 0x00);
    CHECK_EQ(mem.read8(0x00f80000), 0xa5u);
}

TEST_CASE("device space is never treated as memory") {
    GuestMemory mem(0x00ffffff);
    mem.add_ram(0x000000, 0x080000, RegionKind::Chip);
    RecordingDevice custom;
    mem.map_device(0x00dff000, GuestMemory::kPageSize, &custom, RegionKind::Custom,
                   "custom");

    CHECK_EQ(mem.read16(0x00dff004), 0x1234u);
    CHECK_EQ(custom.reads.size(), 1u);
    CHECK_EQ(custom.reads[0].addr, 0x00dff004u);

    mem.write16(0x00dff096, 0x8200);
    CHECK_EQ(custom.writes.size(), 1u);
    CHECK_EQ(custom.writes[0].addr, 0x00dff096u);
    CHECK_EQ(custom.writes[0].value, 0x8200u);

    // A write then a read must not return the written value: these are not
    // storage locations.
    CHECK_EQ(mem.read16(0x00dff096), 0x1234u);
    CHECK(!mem.is_ram(0x00dff000));
    CHECK(mem.kind_of(0x00dff000) == RegionKind::Custom);
}

TEST_CASE("a long access to device space becomes two word cycles") {
    GuestMemory mem(0x00ffffff);
    RecordingDevice custom;
    mem.map_device(0x00dff000, GuestMemory::kPageSize, &custom, RegionKind::Custom);
    mem.write32(0x00dff040, 0x11112222);
    CHECK_EQ(custom.writes.size(), 2u);
    CHECK_EQ(custom.writes[0].addr, 0x00dff040u);
    CHECK_EQ(custom.writes[0].value, 0x1111u);
    CHECK_EQ(custom.writes[1].addr, 0x00dff042u);
    CHECK_EQ(custom.writes[1].value, 0x2222u);
}

TEST_CASE("loading an image reports failure rather than writing nowhere") {
    auto mem = make_memory();
    const u8 data[4] = {1, 2, 3, 4};
    CHECK(mem.load(0x2000, data, sizeof data));
    CHECK_EQ(mem.read32(0x2000), 0x01020304u);
    CHECK(!mem.load(0x400000, data, sizeof data));
}

TEST_CASE("peek and poke never fault, for debugger use") {
    auto mem = make_memory();
    mem.poke16(0x3000, 0xcafe);
    CHECK_EQ(mem.peek16(0x3000), 0xcafeu);
    CHECK_EQ(mem.peek16(0x3001), 0xfe00u);   // unaligned, but no exception
    CHECK_EQ(mem.peek8(0x400000), 0xffu);    // unmapped, but no exception
}

ARECOMP_TEST_MAIN()
