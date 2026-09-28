// The whole stack in one test: 68000 code programs the custom chips, the
// chipset produces a frame, and the frame contains what the program drew.
//
// This is the integration counterpart to the unit tests, which drive the
// chipset registers directly. Here the registers are written by guest code.

#include "amiga_recomp/chipset.hpp"
#include "amiga_recomp/halt_device.hpp"
#include "amiga_recomp/interpreter.hpp"
#include "amiga_recomp/runtime.hpp"

#include <cstdio>
#include <vector>

#include "test_support.hpp"

using namespace arecomp;
using namespace arecomp::test;

namespace {

constexpr u32 kHalt = 0x00f00000;
constexpr u32 kStackTop = 0x0007f000;

struct Demo {
    GuestMemory memory{0x00ffffff};
    M68kState cpu;
    Runtime runtime{memory};
    HaltDevice halt{cpu};
    Chipset chipset;

    explicit Demo(ChipsetConfig config) : chipset(memory, runtime, config) {
        memory.add_ram(0, 0x00080000, RegionKind::Chip, PERM_RWX, "chip");
        memory.map_device(kHalt, GuestMemory::kPageSize, &halt,
                          RegionKind::Expansion, "halt");
        chipset.map(memory);
        runtime.set_hardware(&chipset);
        runtime.set_interpreter(&interpret_block);
        runtime.config().log_unknown_targets = false;

        std::FILE* file =
            std::fopen((std::string(ARECOMP_TESTROM_DIR) + "/copper_demo.bin").c_str(),
                       "rb");
        if (!file) {
            report_failure(__FILE__, __LINE__, "copper_demo.bin not found");
            return;
        }
        std::vector<u8> data;
        u8 buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0)
            data.insert(data.end(), buffer, buffer + n);
        std::fclose(file);
        memory.load(0x1000, data.data(), data.size());

        cpu.pc = 0x1000;
        cpu.set_sr(0x2700);
        cpu.a[7] = kStackTop;
    }

    void run() {
        u64 steps = 0;
        while (!cpu.halted && steps < 20000000) {
            runtime.step_block(cpu);
            ++steps;
        }
    }
};

ChipsetConfig plain() { return ChipsetConfig{}; }

ChipsetConfig wide() {
    ChipsetConfig config;
    config.widescreen.enabled = true;
    config.widescreen.extra_left = 32;
    config.widescreen.extra_right = 32;
    return config;
}

constexpr u32 kWhite = 0xffffffffu;

} // namespace

TEST_CASE("the demo program runs to completion and programs the chipset") {
    Demo demo{plain()};
    demo.run();

    CHECK(demo.cpu.halted);
    CHECK(demo.chipset.frames_completed() >= 2);

    // The registers the program set, read back from the chipset.
    CHECK_EQ(demo.chipset.peek_register(0x100), 0x1000u);   // BPLCON0: one plane
    CHECK_EQ(demo.chipset.peek_register(0x108), 8u);        // BPL1MOD
    CHECK_EQ(demo.chipset.peek_register(0x08e), 0x2c91u);   // DIWSTRT
    CHECK_EQ(demo.chipset.peek_register(0x092), 0x0040u);   // DDFSTRT
    CHECK_EQ(demo.chipset.peek_register(0x096) & 0x0380u, 0x0380u);  // DMACON
}

TEST_CASE("the program's bitmap actually reached chip ram") {
    Demo demo{plain()};
    demo.run();
    // Left margin solid, middle striped, right margin solid.
    CHECK_EQ(demo.memory.peek16(0x00020000), 0xffffu);
    CHECK_EQ(demo.memory.peek16(0x00020004), 0x8181u);
    CHECK_EQ(demo.memory.peek16(0x00020024), 0xffffu);
    CHECK_EQ(demo.memory.peek16(0x00020026), 0xffffu);
    // And the same on a later row, 40 bytes apart.
    CHECK_EQ(demo.memory.peek16(0x00020000 + 40 * 100), 0xffffu);
}

TEST_CASE("the copper produced its colour gradient") {
    Demo demo{plain()};
    demo.run();

    const Framebuffer& fb = demo.chipset.framebuffer();
    CHECK(fb.width > 0);
    CHECK(fb.height > 0);

    // The background steps through the list down the screen, so the top and
    // the bottom must differ. Sample a gap in the stripe pattern ($8181 sets
    // pixel 0, so pixels 1 to 6 show the background), not a set pixel.
    const u32 top = fb.pixels[static_cast<std::size_t>(2) * fb.width + 6];
    const u32 bottom =
        fb.pixels[static_cast<std::size_t>(fb.height - 2) * fb.width + 6];
    CHECK(top != bottom);
}

TEST_CASE("the playfield is displayed, not just the background") {
    Demo demo{plain()};
    demo.run();

    const Framebuffer& fb = demo.chipset.framebuffer();
    // Colour 1 is white, and the striped middle of the bitmap sets it.
    bool found_white = false;
    for (u32 x = 0; x < fb.width; ++x)
        if (fb.pixels[static_cast<std::size_t>(100) * fb.width + x] == kWhite)
            found_white = true;
    CHECK(found_white);
}

TEST_CASE("without widescreen only the game's own window is shown") {
    Demo demo{plain()};
    demo.run();
    const Framebuffer& fb = demo.chipset.framebuffer();
    CHECK_EQ(fb.width, 512u);    // 256 lores pixels
}

TEST_CASE("with widescreen the spare columns appear") {
    Demo demo{wide()};
    demo.run();

    const Framebuffer& fb = demo.chipset.framebuffer();
    CHECK_EQ(fb.width, 640u);    // 256 + 32 + 32 lores pixels
    CHECK_EQ(demo.chipset.applied_extra_left(), 32u);
    CHECK_EQ(demo.chipset.applied_extra_right(), 32u);

    // The margins the program drew are solid, so every column of the first 64
    // and the last 64 is set.
    const u32* row = fb.pixels.data() + static_cast<std::size_t>(100) * fb.width;
    for (u32 x = 0; x < 64; ++x)
        if (row[x] != kWhite) {
            report_failure(__FILE__, __LINE__,
                           "left margin column " + std::to_string(x) +
                               " is " + hex(row[x]));
            break;
        }
    for (u32 x = fb.width - 64; x < fb.width; ++x)
        if (row[x] != kWhite) {
            report_failure(__FILE__, __LINE__,
                           "right margin column " + std::to_string(x) +
                               " is " + hex(row[x]));
            break;
        }
}

ARECOMP_TEST_MAIN()
