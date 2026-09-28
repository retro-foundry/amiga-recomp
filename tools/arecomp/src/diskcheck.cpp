// `arecomp diskcheck`: read a disk image back through the emulated drive and
// check that what comes out is what went in.
//
// The chipset encodes ADF sectors into MFM and serves them over disk DMA. A
// game's own trackloader then decodes them. If that round trip is lossy, the
// game sees corrupt data and fails in ways that look like anything but a disk
// problem -- so this exercises exactly the path a game uses and compares the
// result against the source image, sector by sector.
//
// The decoder here is written independently of the encoder on purpose. Two
// implementations that share code would agree even when both are wrong.

#include "diskcheck.hpp"

#include <cstdio>
#include <vector>

#include "amiga_recomp/chipset.hpp"
#include "amiga_recomp/runtime.hpp"

namespace arecomp {

namespace {

constexpr u32 kSectorsPerTrack = 11;
constexpr u32 kSectorBytes = 512;
constexpr u32 kBuffer = 0x00040000;   // where the DMA lands
constexpr u16 kSyncWord = 0x4489;

u16 word_at(const std::vector<u8>& buffer, std::size_t offset) {
    if (offset + 1 >= buffer.size()) return 0;
    return static_cast<u16>((buffer[offset] << 8) | buffer[offset + 1]);
}

u32 long_at(const std::vector<u8>& buffer, std::size_t offset) {
    return (static_cast<u32>(word_at(buffer, offset)) << 16) |
           word_at(buffer, offset + 2);
}

// Recombine an odd/even encoded block: the first half holds the odd bits of
// every byte, the second half the even bits.
std::vector<u8> decode_block(const std::vector<u8>& mfm, std::size_t offset,
                             std::size_t bytes) {
    std::vector<u8> out(bytes, 0);
    if (offset + bytes * 2 > mfm.size()) return out;
    for (std::size_t i = 0; i < bytes; ++i) {
        const u8 odd = mfm[offset + i];
        const u8 even = mfm[offset + bytes + i];
        u8 value = 0;
        for (int bit = 0; bit < 4; ++bit) {
            value |= static_cast<u8>(((odd >> (2 * bit)) & 1) << (2 * bit + 1));
            value |= static_cast<u8>(((even >> (2 * bit)) & 1) << (2 * bit));
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

u32 decoded_long(const std::vector<u8>& mfm, std::size_t offset) {
    const std::vector<u8> bytes = decode_block(mfm, offset, 4);
    return (static_cast<u32>(bytes[0]) << 24) | (static_cast<u32>(bytes[1]) << 16) |
           (static_cast<u32>(bytes[2]) << 8) | bytes[3];
}

struct Sector {
    bool header_ok = false;
    bool data_ok = false;
    u32 track = 0;
    u32 sector = 0;
    std::vector<u8> data;
};

Sector decode_sector(const std::vector<u8>& mfm, std::size_t offset) {
    Sector out;
    if (offset + 8 + 32 + 8 + 8 + 1024 > mfm.size()) return out;

    const std::vector<u8> info = decode_block(mfm, offset, 4);
    const u32 header_sum = checksum(mfm, offset, 8 + 32);
    const std::size_t sums = offset + 8 + 32;
    const u32 stored_header = decoded_long(mfm, sums);
    const u32 stored_data = decoded_long(mfm, sums + 8);
    const std::size_t data_offset = sums + 16;
    const u32 data_sum = checksum(mfm, data_offset, 1024);

    out.track = info[1];
    out.sector = info[2];
    out.header_ok = info[0] == 0xff && header_sum == stored_header;
    out.data_ok = data_sum == stored_data;
    out.data = decode_block(mfm, data_offset, 512);
    return out;
}

} // namespace

int disk_check(const std::string& path, int only_cylinder, bool verbose) {
    std::vector<u8> adf;
    std::string error;
    if (!read_file(path, adf, error)) {
        std::fprintf(stderr, "arecomp: %s\n", error.c_str());
        return 1;
    }

    const u32 tracks = static_cast<u32>(adf.size() / (kSectorsPerTrack * kSectorBytes));
    std::printf("disk check: %s\n", path.c_str());
    std::printf("  %zu bytes, %u tracks\n\n", adf.size(), tracks);

    GuestMemory memory(0x00ffffff);
    memory.add_ram(0, 0x00100000, RegionKind::Chip, PERM_RWX, "chip");
    Runtime runtime(memory);
    Chipset chipset(memory, runtime, ChipsetConfig{});
    chipset.map(memory);
    runtime.set_hardware(&chipset);
    chipset.insert_disk(adf);

    auto poke = [&](u32 offset, u16 value) {
        memory.write16(kCustomBase + offset, value);
    };
    auto poke_cia = [&](u32 address, u8 value) { memory.write8(address, value); };

    // Select drive zero with the motor running, exactly as a loader does.
    constexpr u8 kSelected = static_cast<u8>(~0x88u & 0xff);
    poke_cia(0x00bfd100, kSelected);

    u64 now = 0;
    auto run_lines = [&](u32 count) {
        now += static_cast<u64>(count) * kColourClocksPerLinePal *
               kCpuCyclesPerColourClock;
        chipset.run_until(now);
    };

    u32 checked = 0, header_failures = 0, data_failures = 0, mismatches = 0,
        missing = 0;

    const u32 first = only_cylinder >= 0 ? static_cast<u32>(only_cylinder) : 0;
    const u32 last = only_cylinder >= 0 ? static_cast<u32>(only_cylinder) : 79;

    for (u32 cylinder = first; cylinder <= last && cylinder < 80; ++cylinder) {
        // Seek: step outward to track zero, then inward to the target.
        while (chipset.disk_cylinder() > 0) {
            poke_cia(0x00bfd100, static_cast<u8>((kSelected | 0x02) | 0x01));
            poke_cia(0x00bfd100, static_cast<u8>((kSelected | 0x02) & ~0x01));
        }
        for (u32 i = 0; i < cylinder; ++i) {
            poke_cia(0x00bfd100, static_cast<u8>((kSelected & ~0x02) | 0x01));
            poke_cia(0x00bfd100, static_cast<u8>((kSelected & ~0x02) & ~0x01));
        }

        for (u32 side = 0; side < 2; ++side) {
            // /SIDE is active low: clearing bit 2 selects the upper head.
            const u8 lines = side == 1 ? static_cast<u8>(kSelected & ~0x04)
                                       : static_cast<u8>(kSelected | 0x04);
            poke_cia(0x00bfd100, lines);

            const u32 track = cylinder * 2 + side;
            if (track >= tracks) continue;

            constexpr u16 kWords = 6400;   // a whole track and then some
            poke(reg::DSKSYNC, kSyncWord);
            poke(reg::ADKCON, 0x8400);     // set WORDSYNC
            poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_DISK);
            poke(reg::DSKPTH, static_cast<u16>(kBuffer >> 16));
            poke(reg::DSKPTL, static_cast<u16>(kBuffer));
            poke(reg::INTREQ, INTF_DSKBLK);
            poke(reg::DSKLEN, 0x4000);
            poke(reg::DSKLEN, static_cast<u16>(0x8000 | kWords));
            poke(reg::DSKLEN, static_cast<u16>(0x8000 | kWords));

            for (int i = 0; i < 200 &&
                            (memory.read16(kCustomBase + reg::INTREQR) & INTF_DSKBLK) == 0;
                 ++i)
                run_lines(1);

            std::vector<u8> mfm(static_cast<std::size_t>(kWords) * 2);
            for (u32 i = 0; i < kWords; ++i) {
                const u16 value = memory.peek16(kBuffer + i * 2);
                mfm[i * 2] = static_cast<u8>(value >> 8);
                mfm[i * 2 + 1] = static_cast<u8>(value);
            }

            // Walk the stream the way a loader does.
            std::vector<bool> seen(kSectorsPerTrack, false);
            for (std::size_t i = 0; i + 1 < mfm.size(); ++i) {
                if (word_at(mfm, i) != kSyncWord) continue;
                if (word_at(mfm, i + 2) == kSyncWord) continue;   // second of a pair
                const Sector sector = decode_sector(mfm, i + 2);
                if (!sector.header_ok) continue;
                if (sector.sector >= kSectorsPerTrack) continue;
                if (seen[sector.sector]) continue;
                seen[sector.sector] = true;
                ++checked;

                if (!sector.data_ok) ++data_failures;

                const std::size_t source =
                    (static_cast<std::size_t>(track) * kSectorsPerTrack +
                     sector.sector) *
                    kSectorBytes;
                bool same = sector.track == track;
                for (std::size_t b = 0; same && b < kSectorBytes; ++b)
                    if (sector.data[b] != adf[source + b]) same = false;
                if (!same) {
                    ++mismatches;
                    if (verbose)
                        std::printf("  track %3u sector %2u: CONTENT MISMATCH "
                                    "(header says track %u)\n",
                                    track, sector.sector, sector.track);
                }
            }

            for (u32 s = 0; s < kSectorsPerTrack; ++s) {
                if (!seen[s]) {
                    ++missing;
                    if (verbose)
                        std::printf("  track %3u sector %2u: NOT FOUND\n", track, s);
                }
            }
        }
    }

    std::printf("sectors decoded      : %u\n", checked);
    std::printf("header checksum bad  : %u\n", header_failures);
    std::printf("data checksum bad    : %u\n", data_failures);
    std::printf("content mismatches   : %u\n", mismatches);
    std::printf("sectors not found    : %u\n", missing);

    const bool ok = data_failures == 0 && mismatches == 0 && missing == 0;
    std::printf("\n%s\n",
                ok ? "The emulated drive returns exactly what is on the disk."
                   : "The round trip is lossy: a game reading this disk sees "
                     "corrupt data.");
    return ok ? 0 : 1;
}

} // namespace arecomp
