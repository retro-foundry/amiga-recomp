// Floppy disk: MFM track images and disk DMA (AMIGA_RECOMP.md 4.3).
//
// Most Amiga games abandon trackdisk.device and read raw MFM themselves,
// decoding sectors and checking their own checksums. To boot one of those from
// an ADF, the runtime has to present the disk the way the hardware does: as a
// stream of MFM, synchronised on DSKSYNC, delivered by DMA.
//
// So the ADF's 512-byte sectors are encoded into standard Amiga MFM tracks on
// demand, and the game's own loader decodes them back. Nothing here knows
// anything about any particular loader.

#include <cstring>

#include "amiga_recomp/chipset.hpp"

namespace arecomp {

namespace {

constexpr u32 kDmaAddressMask = 0x001ffffe;
constexpr u32 kSectorsPerTrack = 11;
constexpr u32 kSectorBytes = 512;
constexpr u32 kTracks = 160;                 // 80 cylinders, two heads
constexpr u16 kSyncWord = 0x4489;

// Builds an MFM bit stream. Each data bit becomes a clock bit followed by the
// data bit, with the clock set only when neither the previous nor the current
// data bit is set. Sync words are written raw, because their whole purpose is
// to violate that rule and so be unmistakable in the stream.
class MfmWriter {
public:
    explicit MfmWriter(std::vector<u8>& out) : out_(out) {}

    void data_bit(bool bit) {
        const bool clock = !last_data_bit_ && !bit;
        raw_bit(clock);
        raw_bit(bit);
        last_data_bit_ = bit;
    }

    // The odd half of a buffer: bits 7, 5, 3 and 1 of every byte.
    void odd_bits(const u8* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i)
            for (int bit = 7; bit >= 1; bit -= 2)
                data_bit(((data[i] >> bit) & 1) != 0);
    }

    // The even half: bits 6, 4, 2 and 0.
    void even_bits(const u8* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i)
            for (int bit = 6; bit >= 0; bit -= 2)
                data_bit(((data[i] >> bit) & 1) != 0);
    }

    void block(const u8* data, std::size_t size) {
        odd_bits(data, size);
        even_bits(data, size);
    }

    void raw_word(u16 value) {
        for (int bit = 15; bit >= 0; --bit) raw_bit(((value >> bit) & 1) != 0);
        // A raw word deliberately breaks the clock rule, so the running state
        // has to follow the bits actually written.
        last_data_bit_ = (value & 1) != 0;
    }

    [[nodiscard]] std::size_t size() const { return out_.size(); }

private:
    void raw_bit(bool bit) {
        if (bit_ == 0) out_.push_back(0);
        if (bit) out_.back() |= static_cast<u8>(0x80 >> bit_);
        bit_ = (bit_ + 1) & 7;
    }

    std::vector<u8>& out_;
    unsigned bit_ = 0;
    bool last_data_bit_ = false;
};

u32 be32_at(const std::vector<u8>& buffer, std::size_t offset) {
    return (static_cast<u32>(buffer[offset]) << 24) |
           (static_cast<u32>(buffer[offset + 1]) << 16) |
           (static_cast<u32>(buffer[offset + 2]) << 8) |
           static_cast<u32>(buffer[offset + 3]);
}

// The Amiga checksum is the exclusive-or of the encoded longs, keeping only
// the data bits. It is computed over the MFM as written, which is why it can
// only be taken after the bytes exist.
u32 mfm_checksum(const std::vector<u8>& buffer, std::size_t begin, std::size_t end) {
    u32 sum = 0;
    for (std::size_t i = begin; i + 4 <= end; i += 4) sum ^= be32_at(buffer, i);
    return sum & 0x55555555u;
}

} // namespace

void Chipset::insert_disk(std::vector<u8> adf) {
    disk_.image = std::move(adf);
    disk_.track_index = 0xffffffffu;
    disk_.track.clear();
    disk_.cylinder = 0;
    disk_.side = 0;
    disk_.change_flag = true;   // a new disk is, by definition, a change
}

void Chipset::eject_disk() {
    disk_ = Disk{};
}

void Chipset::disk_build_track() {
    const u32 index = disk_.cylinder * 2 + disk_.side;
    if (index == disk_.track_index && !disk_.track.empty()) return;
    disk_.track_index = index;
    disk_.track.clear();

    if (disk_.image.empty() || index >= kTracks) return;

    const std::size_t track_offset =
        static_cast<std::size_t>(index) * kSectorsPerTrack * kSectorBytes;
    if (track_offset + kSectorsPerTrack * kSectorBytes > disk_.image.size()) return;

    disk_.track.reserve(kSectorsPerTrack * 1088 + 512);
    MfmWriter writer(disk_.track);

    for (u32 sector = 0; sector < kSectorsPerTrack; ++sector) {
        // Two encoded zero bytes of gap, then the sync pattern.
        const u8 gap[2] = {0, 0};
        writer.block(gap, 1);
        writer.raw_word(kSyncWord);
        writer.raw_word(kSyncWord);

        // Header: format, track, sector, and how many sectors remain before
        // the gap. Games use the last field to know where the track wraps.
        const u8 info[4] = {0xff, static_cast<u8>(index), static_cast<u8>(sector),
                            static_cast<u8>(kSectorsPerTrack - sector)};
        const u8 label[16] = {};

        const std::size_t header_begin = disk_.track.size();
        writer.block(info, sizeof info);
        writer.block(label, sizeof label);
        const std::size_t header_end = disk_.track.size();

        // The data has to exist before its checksum can be written, but the
        // checksum comes first in the stream. Encode the data to the side,
        // then splice.
        std::vector<u8> encoded_data;
        encoded_data.reserve(kSectorBytes * 2);
        {
            MfmWriter data_writer(encoded_data);
            data_writer.block(disk_.image.data() + track_offset + sector * kSectorBytes,
                              kSectorBytes);
        }

        const u32 header_sum = mfm_checksum(disk_.track, header_begin, header_end);
        const u32 data_sum = mfm_checksum(encoded_data, 0, encoded_data.size());

        const u8 header_sum_bytes[4] = {
            static_cast<u8>(header_sum >> 24), static_cast<u8>(header_sum >> 16),
            static_cast<u8>(header_sum >> 8), static_cast<u8>(header_sum)};
        const u8 data_sum_bytes[4] = {
            static_cast<u8>(data_sum >> 24), static_cast<u8>(data_sum >> 16),
            static_cast<u8>(data_sum >> 8), static_cast<u8>(data_sum)};

        writer.block(header_sum_bytes, sizeof header_sum_bytes);
        writer.block(data_sum_bytes, sizeof data_sum_bytes);
        disk_.track.insert(disk_.track.end(), encoded_data.begin(), encoded_data.end());
    }

    // A real track is about 12800 MFM bytes; eleven sectors account for
    // 11968 of them and the rest is gap. Loaders that read a whole track in
    // one transfer rely on the track being that long, so the gap is not
    // optional padding.
    constexpr std::size_t kTrackBytes = 12800;
    while (disk_.track.size() < kTrackBytes) disk_.track.push_back(0xaa);
}

// CIA-B port B carries the drive control lines, all active low.
void Chipset::disk_write_ciab_prb(u8 value) {
    const bool motor_on = (value & 0x80) == 0;      // /MTR
    const bool selected = (value & 0x08) == 0;      // /SEL0, the internal drive
    const bool step = (value & 0x01) != 0;          // /STEP
    const bool direction_out = (value & 0x02) != 0; // DIR: towards track 0

    // The motor latch is sampled when a drive is selected.
    if (selected) disk_.motor = motor_on;
    disk_.selected = selected;
    disk_.direction_out = direction_out;

    // A high-to-low transition on /STEP moves the head one cylinder.
    if (disk_.step_line && !step && selected) {
        if (direction_out) {
            if (disk_.cylinder > 0) --disk_.cylinder;
        } else {
            if (disk_.cylinder < 79) ++disk_.cylinder;
        }
        disk_.track_index = 0xffffffffu;   // the head moved: rebuild on demand
        // Stepping with a disk in the drive acknowledges the change.
        if (!disk_.image.empty()) disk_.change_flag = false;
    }
    disk_.step_line = step;

    // /SIDE selects the head: low means the upper one.
    disk_.side = (value & 0x04) ? 0u : 1u;
}

// CIA-A port A reports the drive's status lines, also active low.
u8 Chipset::disk_status_bits() const {
    u8 bits = 0x3c;    // /CHNG, /WPROT, /TK0 and /RDY all high
    if (disk_.cylinder == 0) bits = static_cast<u8>(bits & ~0x10);   // /TK0
    if (disk_.motor && disk_.selected && !disk_.image.empty())
        bits = static_cast<u8>(bits & ~0x20);                        // /RDY
    // /CHNG is low while a change is pending, which is the case until the
    // head steps, and permanently when the drive is empty.
    if (disk_.change_flag || disk_.image.empty())
        bits = static_cast<u8>(bits & ~0x04);
    return bits;
}

void Chipset::disk_start_dma() {
    const u16 length = regs_[reg::DSKLEN >> 1];
    if (!(length & 0x8000)) {          // DMAEN clear: this disarms the transfer
        disk_.length_written = 0;
        disk_.dma_active = false;
        return;
    }
    // DSKLEN has to be written twice with DMAEN set, which is how the hardware
    // avoids starting a transfer on a stray write.
    if (disk_.length_written == 0 || disk_.last_length != length) {
        disk_.length_written = 1;
        disk_.last_length = length;
        return;
    }
    disk_.length_written = 0;

    if (length & 0x4000) return;       // writing to disk is not supported
    disk_build_track();
    if (disk_.track.empty()) return;

    disk_.words_remaining = length & 0x3fff;
    disk_.position = 0;

    // With WORDSYNC the transfer does not begin until DSKSYNC appears in the
    // stream, and the sync word itself is not delivered.
    if (regs_[reg::ADKCON >> 1] & 0x0400) {
        const u16 sync = regs_[reg::DSKSYNC >> 1];
        bool found = false;
        for (std::size_t i = 0; i + 1 < disk_.track.size(); ++i) {
            const u16 word = static_cast<u16>((disk_.track[i] << 8) | disk_.track[i + 1]);
            if (word == sync) {
                disk_.position = static_cast<u32>(i + 2);
                found = true;
                break;
            }
        }
        if (!found) return;            // no sync: the transfer never completes
        raise_interrupt(INTF_DSKSYN);
    }

    disk_.dma_active = true;
    ++disk_.reads;
}

void Chipset::disk_step() {
    if (!disk_.dma_active) return;
    const u16 dmacon = regs_[reg::DMACON >> 1];
    if (!(dmacon & DMAF_MASTER) || !(dmacon & DMAF_DISK)) return;

    // One word per colour clock is far faster than a real drive, but nothing
    // in a loader depends on the transfer being slow, and the completion
    // interrupt still arrives asynchronously.
    u32 pointer = ((static_cast<u32>(regs_[reg::DSKPTH >> 1]) << 16) |
                   regs_[reg::DSKPTL >> 1]) & kDmaAddressMask;

    const std::size_t size = disk_.track.size();
    u16 word = 0;
    if (disk_.position + 1 < size) {
        word = static_cast<u16>((disk_.track[disk_.position] << 8) |
                                disk_.track[disk_.position + 1]);
    }
    disk_.position += 2;
    if (disk_.position >= size) disk_.position = 0;   // the track loops

    memory_.poke16(pointer, word);
    pointer = (pointer + 2) & kDmaAddressMask;
    regs_[reg::DSKPTH >> 1] = static_cast<u16>(pointer >> 16);
    regs_[reg::DSKPTL >> 1] = static_cast<u16>(pointer);

    if (--disk_.words_remaining == 0) {
        disk_.dma_active = false;
        raise_interrupt(INTF_DSKBLK);
    }
}

} // namespace arecomp
