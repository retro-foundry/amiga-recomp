// Widescreen: revealing the columns a game already draws but never displays
// (AMIGA_RECOMP.md 37 level 1, docs/widescreen.md).
//
// The setup here is the one most Amiga side-scrollers use: a bitmap wider than
// the display window, with the bitplane pointer somewhere inside each row and
// a positive BPL1MOD taking up the difference. Those spare columns are drawn
// every frame and simply never shown. Widening the fetch and display windows
// shows them.
//
// These tests check the two things that make the difference between widescreen
// and a torn picture: the bitplane pointers walk back for the left extension,
// and the modulo is compensated so the row stride does not change.

#include "amiga_recomp/chipset.hpp"
#include "amiga_recomp/runtime.hpp"

#include "test_support.hpp"

using namespace arecomp;
using namespace arecomp::test;

namespace {

constexpr u32 kChipRam = 0x00080000;
constexpr u32 kBitplane = 0x00020000;

// A bitmap 320 lores pixels wide (20 words, 40 bytes per row), displayed 256
// pixels wide (16 words). The pointer starts two words into each row, so there
// are 32 spare pixels on the left and 32 on the right.
//
// A 256-pixel window is used rather than 320 because a PAL scan line holds
// only 454 lores pixels, and the standard 320-pixel window sits at 129..449,
// which leaves no room on the right at all. That limit is real and is the
// reason level 1 widescreen tops out near 370 pixels; see docs/widescreen.md.
constexpr u32 kRowBytes = 40;
constexpr u32 kVisibleWords = 16;
constexpr u32 kLeftSlackWords = 2;
constexpr u32 kRows = 220;

struct Machine {
    GuestMemory memory{0x00ffffff};
    M68kState cpu;
    Runtime runtime{memory};
    Chipset chipset;
    u64 now = 0;

    explicit Machine(ChipsetConfig config) : chipset(memory, runtime, config) {
        memory.add_ram(0, kChipRam, RegionKind::Chip, PERM_RWX, "chip");
        chipset.map(memory);
        runtime.set_hardware(&chipset);
    }

    void poke(u32 offset, u16 value) { memory.write16(kCustomBase + offset, value); }

    void run_frames(u32 count) {
        now += static_cast<u64>(count) * kLinesPerFramePal * kColourClocksPerLinePal *
               kCpuCyclesPerColourClock;
        chipset.run_until(now);
    }

    void setup() {
        // Word 0 and 1 are the left slack, 2..21 the visible window, 22 and 23
        // the right slack. Marking each region differently makes it obvious
        // which columns ended up on screen.
        for (u32 row = 0; row < kRows; ++row) {
            const u32 base = kBitplane + row * kRowBytes;
            for (u32 word = 0; word < 20; ++word) {
                u16 value = 0x0000;                      // visible: clear
                if (word < kLeftSlackWords) value = 0xffff;   // left slack: set
                if (word >= kLeftSlackWords + kVisibleWords) value = 0xffff;  // right
                memory.write16(base + word * 2, value);
            }
        }

        // A 256-pixel window at lores 145..401, with the fetch window lined
        // up to match: 16 words, starting one pipeline delay earlier.
        poke(reg::DIWSTRT, 0x2c91);
        poke(reg::DIWSTOP, 0xf491);
        poke(reg::DDFSTRT, 0x0040);
        poke(reg::DDFSTOP, 0x00b8);
        poke(reg::BPLCON0, 1 << BPLCON0_BPU_SHIFT);
        poke(reg::BPLCON1, 0);
        // The row stride is 48 bytes and 40 are fetched, so the modulo is 8.
        poke(reg::BPL1MOD, static_cast<u16>(kRowBytes - kVisibleWords * 2));
        // The pointer starts after the left slack.
        const u32 start = kBitplane + kLeftSlackWords * 2;
        poke(reg::BPL1PTH, static_cast<u16>(start >> 16));
        poke(reg::BPL1PTL, static_cast<u16>(start));
        poke(reg::COLOR00, 0x0000);
        poke(reg::COLOR00 + 2, 0x0fff);
        poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_RASTER);
    }
};

constexpr u32 kWhite = 0xffffffffu;
constexpr u32 kBlack = 0xff000000u;

ChipsetConfig widescreen_config(u32 left, u32 right, bool compensate = true) {
    ChipsetConfig config;
    config.widescreen.enabled = true;
    config.widescreen.extra_left = left;
    config.widescreen.extra_right = right;
    config.widescreen.compensate_modulo = compensate;
    return config;
}

} // namespace

TEST_CASE("without widescreen the game's own window is what is shown") {
    Machine m{ChipsetConfig{}};
    m.setup();
    m.run_frames(2);

    const Framebuffer& fb = m.chipset.framebuffer();
    CHECK_EQ(fb.width, 512u);      // 256 lores pixels, two columns each
    CHECK_EQ(fb.height, 200u);

    // Every visible pixel comes from the cleared middle of the bitmap.
    const u32* row = fb.pixels.data() + static_cast<std::size_t>(100) * fb.width;
    CHECK_EQ(row[0], kBlack);
    CHECK_EQ(row[fb.width / 2], kBlack);
    CHECK_EQ(row[fb.width - 1], kBlack);
}

TEST_CASE("widescreen reveals the columns the game already draws") {
    Machine m{widescreen_config(32, 32)};
    m.setup();
    m.run_frames(2);

    const Framebuffer& fb = m.chipset.framebuffer();
    // 256 + 32 + 32 = 320 lores pixels.
    CHECK_EQ(fb.width, 640u);
    CHECK_EQ(fb.height, 200u);
    CHECK_EQ(m.chipset.applied_extra_left(), 32u);
    CHECK_EQ(m.chipset.applied_extra_right(), 32u);

    const u32* row = fb.pixels.data() + static_cast<std::size_t>(100) * fb.width;
    // The first 32 lores pixels are the left slack, so 64 framebuffer columns.
    CHECK_EQ(row[0], kWhite);
    CHECK_EQ(row[63], kWhite);
    // Then the game's own window, which is clear.
    CHECK_EQ(row[64], kBlack);
    CHECK_EQ(row[fb.width / 2], kBlack);
    CHECK_EQ(row[575], kBlack);
    // And the right slack.
    CHECK_EQ(row[576], kWhite);
    CHECK_EQ(row[639], kWhite);
}

TEST_CASE("the row stride is preserved, so the picture does not shear") {
    // This is the check that matters. If the modulo is not compensated, each
    // line starts four words further on than it should and the image skews
    // steadily across the screen.
    Machine m{widescreen_config(32, 32)};
    m.setup();
    m.run_frames(2);

    const Framebuffer& fb = m.chipset.framebuffer();
    for (u32 y = 0; y < fb.height; y += 20) {
        const u32* row = fb.pixels.data() + static_cast<std::size_t>(y) * fb.width;
        if (row[0] != kWhite)
            report_failure(__FILE__, __LINE__,
                           "left margin wrong on line " + std::to_string(y));
        if (row[64] != kBlack)
            report_failure(__FILE__, __LINE__,
                           "window start wrong on line " + std::to_string(y));
        if (row[639] != kWhite)
            report_failure(__FILE__, __LINE__,
                           "right margin wrong on line " + std::to_string(y));
    }
}

TEST_CASE("without modulo compensation the picture shears, as it should") {
    // Proving the compensation is load bearing rather than decorative.
    Machine m{widescreen_config(32, 32, /*compensate=*/false)};
    m.setup();
    m.run_frames(2);

    // Each line starts eight bytes further into the bitmap than it should,
    // so consecutive rows no longer line up. Rows forty apart would drift by
    // a whole multiple of the row stride and coincidentally realign, which is
    // exactly the kind of check that passes for the wrong reason.
    const Framebuffer& fb = m.chipset.framebuffer();
    const u32* first = fb.pixels.data();
    const u32* second = fb.pixels.data() + static_cast<std::size_t>(1) * fb.width;
    bool differs = false;
    for (u32 x = 0; x < fb.width; ++x)
        if (first[x] != second[x]) differs = true;
    CHECK(differs);
}

TEST_CASE("the extension is rounded down to whole fetch blocks") {
    // A fetch block is one word per plane: 16 lores pixels. Asking for 40
    // extra pixels gets 32, because a partial block cannot be fetched.
    Machine m{widescreen_config(40, 40)};
    m.setup();
    m.run_frames(2);
    CHECK_EQ(m.chipset.framebuffer().width, 640u);   // 256 + 32 + 32, doubled
}

TEST_CASE("asking for nothing changes nothing") {
    Machine m{widescreen_config(0, 0)};
    m.setup();
    m.run_frames(2);
    CHECK_EQ(m.chipset.framebuffer().width, 512u);
}

TEST_CASE("slack_words reports how much room the game's modulo leaves") {
    // A port uses this to ask for what is really there instead of guessing.
    Machine m{ChipsetConfig{}};
    m.setup();
    m.run_frames(1);
    // BPL1MOD is 8 bytes, which is four words, which is 64 lores pixels.
    CHECK_EQ(m.chipset.slack_words(0), 4);
}

TEST_CASE("a game with no slack cannot be widened without shearing") {
    // The honest case: a bitmap exactly as wide as the display window has
    // nothing spare, and the framework must not pretend otherwise.
    Machine m{ChipsetConfig{}};
    m.setup();
    m.poke(reg::BPL1MOD, 0);
    m.run_frames(1);
    CHECK_EQ(m.chipset.slack_words(0), 0);
}

TEST_CASE("widescreen can be turned on and off between frames") {
    Machine m{ChipsetConfig{}};
    m.setup();
    m.run_frames(2);
    CHECK_EQ(m.chipset.framebuffer().width, 512u);

    m.chipset.widescreen().enabled = true;
    m.chipset.widescreen().extra_left = 32;
    m.chipset.widescreen().extra_right = 32;
    m.run_frames(2);
    CHECK_EQ(m.chipset.framebuffer().width, 640u);

    m.chipset.widescreen().enabled = false;
    m.run_frames(2);
    CHECK_EQ(m.chipset.framebuffer().width, 512u);
}

ARECOMP_TEST_MAIN()
