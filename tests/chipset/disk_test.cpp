// Floppy disk: MFM track encoding and disk DMA.
//
// The encoder is checked against a decoder written independently here, doing
// what a game's own loader does: find the sync, split the odd and even halves,
// verify the checksums, and reassemble the sector. If the two agreed only
// because they share code, the test would be worthless.

#include "amiga_recomp/chipset.hpp"
#include "amiga_recomp/runtime.hpp"

#include <vector>

#include "test_support.hpp"

using namespace arecomp;
using namespace arecomp::test;

namespace {

constexpr u32 kSectorsPerTrack = 11;
constexpr u32 kSectorBytes = 512;
constexpr u32 kDestination = 0x00030000;

// A disk whose every byte is predictable, so a misplaced sector is obvious.
std::vector<u8> make_test_adf() {
    std::vector<u8> adf(80u * 2u * kSectorsPerTrack * kSectorBytes);
    for (std::size_t i = 0; i < adf.size(); ++i) {
        const std::size_t sector = i / kSectorBytes;
        const std::size_t offset = i % kSectorBytes;
        adf[i] = static_cast<u8>((sector * 7 + offset * 3) & 0xff);
    }
    return adf;
}

struct Machine {
    GuestMemory memory{0x00ffffff};
    M68kState cpu;
    Runtime runtime{memory};
    Chipset chipset;
    u64 now = 0;

    Machine() : chipset(memory, runtime, ChipsetConfig{}) {
        memory.add_ram(0, 0x00080000, RegionKind::Chip, PERM_RWX, "chip");
        chipset.map(memory);
        runtime.set_hardware(&chipset);
    }

    void poke(u32 offset, u16 value) { memory.write16(kCustomBase + offset, value); }
    [[nodiscard]] u16 peek(u32 offset) { return memory.read16(kCustomBase + offset); }
    void poke_cia(u32 address, u8 value) { memory.write8(address, value); }
    [[nodiscard]] u8 peek_cia(u32 address) { return memory.read8(address); }

    void run_lines(u32 count) {
        now += static_cast<u64>(count) * kColourClocksPerLinePal * kCpuCyclesPerColourClock;
        chipset.run_until(now);
    }
};

// --- an independent MFM decoder -------------------------------------------

u16 word_at(const std::vector<u8>& buffer, std::size_t offset) {
    if (offset + 1 >= buffer.size()) return 0;
    return static_cast<u16>((buffer[offset] << 8) | buffer[offset + 1]);
}

u32 long_at(const std::vector<u8>& buffer, std::size_t offset) {
    return (static_cast<u32>(word_at(buffer, offset)) << 16) |
           word_at(buffer, offset + 2);
}

// Recombine an odd/even encoded block: the first half carries the odd bits of
// every byte, the second half the even bits.
std::vector<u8> decode_block(const std::vector<u8>& mfm, std::size_t offset,
                             std::size_t bytes) {
    std::vector<u8> out(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        const u8 odd = mfm[offset + i];
        const u8 even = mfm[offset + bytes + i];
        u8 value = 0;
        for (int bit = 0; bit < 4; ++bit) {
            const u8 odd_bit = (odd >> (2 * bit)) & 1;
            const u8 even_bit = (even >> (2 * bit)) & 1;
            value |= static_cast<u8>(odd_bit << (2 * bit + 1));
            value |= static_cast<u8>(even_bit << (2 * bit));
        }
        out[i] = value;
    }
    return out;
}

u32 checksum(const std::vector<u8>& mfm, std::size_t offset, std::size_t bytes) {
    u32 sum = 0;
    for (std::size_t i = 0; i + 4 <= bytes; i += 4) sum ^= long_at(mfm, offset + i);
    return sum & 0x55555555u;
}

struct DecodedSector {
    bool valid = false;
    u32 track = 0;
    u32 sector = 0;
    std::vector<u8> data;
};

// Decode the sector beginning at `offset`, which must point just past a sync.
DecodedSector decode_sector(const std::vector<u8>& mfm, std::size_t offset) {
    DecodedSector out;
    if (offset + 8 + 32 + 8 + 8 + 1024 > mfm.size()) return out;

    const std::vector<u8> info = decode_block(mfm, offset, 4);
    const std::size_t header_begin = offset;
    const std::size_t header_bytes = 8 + 32;
    const u32 header_sum = checksum(mfm, header_begin, header_bytes);

    const std::size_t sums = offset + 8 + 32;
    const std::vector<u8> stored_header_sum = decode_block(mfm, sums, 4);
    const std::vector<u8> stored_data_sum = decode_block(mfm, sums + 8, 4);

    const std::size_t data_offset = sums + 16;
    const u32 data_sum = checksum(mfm, data_offset, 1024);

    const u32 stored_h = (static_cast<u32>(stored_header_sum[0]) << 24) |
                         (static_cast<u32>(stored_header_sum[1]) << 16) |
                         (static_cast<u32>(stored_header_sum[2]) << 8) |
                         stored_header_sum[3];
    const u32 stored_d = (static_cast<u32>(stored_data_sum[0]) << 24) |
                         (static_cast<u32>(stored_data_sum[1]) << 16) |
                         (static_cast<u32>(stored_data_sum[2]) << 8) |
                         stored_data_sum[3];

    if (info[0] != 0xff) return out;
    if (header_sum != stored_h) return out;
    if (data_sum != stored_d) return out;

    out.valid = true;
    out.track = info[1];
    out.sector = info[2];
    out.data = decode_block(mfm, data_offset, 512);
    return out;
}

// Read the MFM the DMA delivered back out of guest memory.
std::vector<u8> read_transfer(Machine& m, u32 words) {
    std::vector<u8> out;
    out.reserve(words * 2);
    for (u32 i = 0; i < words; ++i) {
        const u16 word = m.memory.peek16(kDestination + i * 2);
        out.push_back(static_cast<u8>(word >> 8));
        out.push_back(static_cast<u8>(word));
    }
    return out;
}

void start_read(Machine& m, u16 words) {
    m.poke(reg::DSKSYNC, 0x4489);
    m.poke(reg::ADKCON, 0x8400);          // set WORDSYNC
    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_DISK);
    m.poke(reg::DSKPTH, static_cast<u16>(kDestination >> 16));
    m.poke(reg::DSKPTL, static_cast<u16>(kDestination));
    m.poke(reg::INTREQ, INTF_DSKBLK);     // clear any previous completion
    m.poke(reg::DSKLEN, 0x4000);          // disarm
    m.poke(reg::DSKLEN, static_cast<u16>(0x8000 | words));   // arm
    m.poke(reg::DSKLEN, static_cast<u16>(0x8000 | words));   // and go
}

} // namespace

TEST_CASE("with no disk inserted nothing is ready") {
    Machine m;
    CHECK(!m.chipset.disk_inserted());
    // /RDY stays high when there is no disk.
    CHECK_EQ(m.peek_cia(0x00bfe001) & 0x20u, 0x20u);
}

TEST_CASE("the drive reports ready once a disk is in and the motor is on") {
    Machine m;
    m.chipset.insert_disk(make_test_adf());
    CHECK(m.chipset.disk_inserted());

    // CIA-B port B: select drive 0 and turn the motor on, both active low.
    m.poke_cia(0x00bfd100, static_cast<u8>(~0x88u & 0xff));
    CHECK_EQ(m.peek_cia(0x00bfe001) & 0x20u, 0u);   // /RDY low
    CHECK_EQ(m.peek_cia(0x00bfe001) & 0x10u, 0u);   // /TK0 low: at track zero
}

// CIA-B port B, all lines active low: bit 7 /MTR, bit 3 /SEL0, bit 2 /SIDE,
// bit 1 DIR (set steps outward, towards track zero), bit 0 /STEP.
constexpr u8 kDriveSelected = static_cast<u8>(~0x88u & 0xff);
constexpr u8 kStepInward = kDriveSelected & ~0x02;
constexpr u8 kStepOutward = kDriveSelected | 0x02;

void pulse_step(Machine& m, u8 lines) {
    m.poke_cia(0x00bfd100, static_cast<u8>(lines | 0x01));
    m.poke_cia(0x00bfd100, static_cast<u8>(lines & ~0x01));
}

TEST_CASE("step pulses move the head and track zero is reported") {
    Machine m;
    m.chipset.insert_disk(make_test_adf());
    m.poke_cia(0x00bfd100, kDriveSelected);

    for (int i = 0; i < 3; ++i) pulse_step(m, kStepInward);
    CHECK_EQ(m.chipset.disk_cylinder(), 3u);
    CHECK_EQ(m.peek_cia(0x00bfe001) & 0x10u, 0x10u);   // no longer at track zero

    for (int i = 0; i < 3; ++i) pulse_step(m, kStepOutward);
    CHECK_EQ(m.chipset.disk_cylinder(), 0u);
    CHECK_EQ(m.peek_cia(0x00bfe001) & 0x10u, 0u);
}

TEST_CASE("the head cannot step past either end of the disk") {
    Machine m;
    m.chipset.insert_disk(make_test_adf());
    m.poke_cia(0x00bfd100, kDriveSelected);

    for (int i = 0; i < 5; ++i) pulse_step(m, kStepOutward);   // already at zero
    CHECK_EQ(m.chipset.disk_cylinder(), 0u);

    for (int i = 0; i < 100; ++i) pulse_step(m, kStepInward);
    CHECK_EQ(m.chipset.disk_cylinder(), 79u);
}

TEST_CASE("a dma read delivers decodable mfm and raises its interrupt") {
    Machine m;
    const std::vector<u8> adf = make_test_adf();
    m.chipset.insert_disk(adf);
    m.poke_cia(0x00bfd100, static_cast<u8>(~0x88u & 0xff));
    m.poke(reg::INTENA, 0x8000 | INTF_INTEN | INTF_DSKBLK);

    constexpr u16 kWords = 6000;
    start_read(m, kWords);

    // The transfer is asynchronous; give it time.
    for (int i = 0; i < 100 && (m.peek(reg::INTREQR) & INTF_DSKBLK) == 0; ++i)
        m.run_lines(1);

    CHECK_EQ(m.peek(reg::INTREQR) & INTF_DSKBLK, INTF_DSKBLK);
    CHECK_EQ(m.chipset.disk_reads(), 1u);

    const std::vector<u8> mfm = read_transfer(m, kWords);

    // The first word after the sync is the second sync of the pair, and the
    // sector header follows it.
    CHECK_EQ(word_at(mfm, 0), 0x4489u);

    const DecodedSector sector = decode_sector(mfm, 2);
    CHECK(sector.valid);
    if (!sector.valid) return;

    CHECK_EQ(sector.track, 0u);
    CHECK_EQ(sector.data.size(), kSectorBytes);

    // And it must be the bytes that are actually on the disk.
    const std::size_t source = sector.sector * kSectorBytes;
    bool matches = true;
    for (std::size_t i = 0; i < kSectorBytes; ++i)
        if (sector.data[i] != adf[source + i]) matches = false;
    CHECK(matches);
}

TEST_CASE("every sector of a track decodes, with correct checksums") {
    Machine m;
    const std::vector<u8> adf = make_test_adf();
    m.chipset.insert_disk(adf);
    m.poke_cia(0x00bfd100, static_cast<u8>(~0x88u & 0xff));

    constexpr u16 kWords = 6900;   // a whole track and then some
    start_read(m, kWords);
    for (int i = 0; i < 200 && m.chipset.disk_reads() > 0 &&
                    (m.peek(reg::INTREQR) & INTF_DSKBLK) == 0;
         ++i)
        m.run_lines(1);

    const std::vector<u8> mfm = read_transfer(m, kWords);

    // Walk the stream the way a loader does: find each sync, decode what
    // follows, and collect the sectors.
    std::vector<bool> seen(kSectorsPerTrack, false);
    u32 decoded = 0;
    for (std::size_t i = 0; i + 1 < mfm.size(); ++i) {
        if (word_at(mfm, i) != 0x4489) continue;
        if (word_at(mfm, i + 2) == 0x4489) continue;   // the second of a pair
        const DecodedSector sector = decode_sector(mfm, i + 2);
        if (!sector.valid) continue;
        ++decoded;
        if (sector.sector < kSectorsPerTrack) {
            seen[sector.sector] = true;
            const std::size_t source = sector.sector * kSectorBytes;
            for (std::size_t b = 0; b < kSectorBytes; ++b) {
                if (sector.data[b] != adf[source + b]) {
                    report_failure(__FILE__, __LINE__,
                                   "sector " + std::to_string(sector.sector) +
                                       " byte " + std::to_string(b) + " wrong");
                    b = kSectorBytes;
                }
            }
        }
    }

    CHECK(decoded >= kSectorsPerTrack);
    for (u32 s = 0; s < kSectorsPerTrack; ++s)
        if (!seen[s])
            report_failure(__FILE__, __LINE__,
                           "sector " + std::to_string(s) + " never appeared");
}

TEST_CASE("a read from a different track returns that track's data") {
    Machine m;
    const std::vector<u8> adf = make_test_adf();
    m.chipset.insert_disk(adf);
    m.poke_cia(0x00bfd100, kDriveSelected);
    for (int i = 0; i < 5; ++i) pulse_step(m, kStepInward);   // to cylinder 5
    CHECK_EQ(m.chipset.disk_cylinder(), 5u);

    constexpr u16 kWords = 3000;
    start_read(m, kWords);
    for (int i = 0; i < 100 && (m.peek(reg::INTREQR) & INTF_DSKBLK) == 0; ++i)
        m.run_lines(1);

    const std::vector<u8> mfm = read_transfer(m, kWords);
    const DecodedSector sector = decode_sector(mfm, 2);
    CHECK(sector.valid);
    // Cylinder 5, head 0 is logical track 10.
    if (sector.valid) CHECK_EQ(sector.track, 10u);
}

TEST_CASE("dsklen must be written twice before a transfer starts") {
    Machine m;
    m.chipset.insert_disk(make_test_adf());
    m.poke_cia(0x00bfd100, static_cast<u8>(~0x88u & 0xff));
    m.poke(reg::DSKSYNC, 0x4489);
    m.poke(reg::ADKCON, 0x8400);
    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_DISK);
    m.poke(reg::DSKPTH, static_cast<u16>(kDestination >> 16));
    m.poke(reg::DSKPTL, static_cast<u16>(kDestination));

    m.poke(reg::DSKLEN, 0x8100);    // armed, but only once
    m.run_lines(2);
    CHECK_EQ(m.chipset.disk_reads(), 0u);

    m.poke(reg::DSKLEN, 0x8100);    // now it goes
    CHECK_EQ(m.chipset.disk_reads(), 1u);
}

ARECOMP_TEST_MAIN()
