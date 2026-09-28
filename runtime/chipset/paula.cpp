// Paula's four audio channels (AMIGA_RECOMP.md 23).
//
// Period counts colour clocks, so a channel's sample rate is
// 3546895 / period on PAL. Each channel fetches one word of sample data per
// two output samples and raises its interrupt when a buffer is exhausted,
// which is how games chain samples and drive music replayers.

#include "amiga_recomp/chipset.hpp"

namespace arecomp {

namespace {
constexpr u32 kDmaAddressMask = 0x001ffffe;
constexpr u16 kAudioInterrupt[4] = {INTF_AUD0, INTF_AUD1, INTF_AUD2, INTF_AUD3};
constexpr u16 kAudioDma[4] = {DMAF_AUD0, DMAF_AUD1, DMAF_AUD2, DMAF_AUD3};
} // namespace

void Chipset::audio_step() {
    const u16 dmacon = regs_[reg::DMACON >> 1];
    const bool master = (dmacon & DMAF_MASTER) != 0;

    for (u32 i = 0; i < 4; ++i) {
        AudioChannel& channel = audio_[i];
        const bool dma = master && (dmacon & kAudioDma[i]) != 0;

        if (dma && !channel.dma_on) {
            // Enabling DMA restarts the channel from its location register.
            channel.pointer = channel.location;
            channel.words_left = channel.length;
            channel.counter = channel.period;
            // The first word is fetched immediately, and the channel raises
            // its interrupt once per buffer start.
            channel.sample = memory_.peek16(channel.pointer);
            channel.pointer = (channel.pointer + 2) & kDmaAddressMask;
            if (channel.words_left > 0) --channel.words_left;
            raise_interrupt(kAudioInterrupt[i]);
        }
        channel.dma_on = dma;
        if (!dma) continue;

        if (--channel.counter > 0) continue;
        channel.counter = channel.period ? channel.period : 1;

        // Two samples per word: high byte first.
        static_assert(sizeof(s8) == 1, "sample bytes are signed 8-bit");
        channel.output = static_cast<s8>(channel.sample >> 8);
        channel.sample = static_cast<u16>(channel.sample << 8);

        // Every second sample consumes a new word.
        if ((channel.counter & 1) == 0) continue;

        if (channel.words_left == 0) {
            // Buffer exhausted: reload and tell the game, which is when a
            // replayer swaps in the next block.
            channel.pointer = channel.location;
            channel.words_left = channel.length;
            raise_interrupt(kAudioInterrupt[i]);
        }
        channel.sample = memory_.peek16(channel.pointer);
        channel.pointer = (channel.pointer + 2) & kDmaAddressMask;
        if (channel.words_left > 0) --channel.words_left;
    }
}

} // namespace arecomp
