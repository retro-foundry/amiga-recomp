// Custom chip behaviour: register semantics, interrupts, the Copper, the
// bitplane display and the blitter.
//
// These cover the hardware items in the spec's success criteria (section 64):
// a Copper program produces the right raster, a bitplane display shows the
// expected image, and a blit modifies Chip RAM and raises its interrupt.

#include "amiga_recomp/chipset.hpp"
#include "amiga_recomp/runtime.hpp"

#include <cstdio>
#include <string>

#include "test_support.hpp"

using namespace arecomp;
using namespace arecomp::test;

namespace {

constexpr u32 kChipRam = 0x00080000;
constexpr u32 kBitplane = 0x00020000;
constexpr u32 kCopperList = 0x00030000;

struct Machine {
    GuestMemory memory{0x00ffffff};
    M68kState cpu;
    Runtime runtime{memory};
    Chipset chipset;

    explicit Machine(ChipsetConfig config = {})
        : chipset(memory, runtime, config) {
        memory.add_ram(0, kChipRam, RegionKind::Chip, PERM_RWX, "chip");
        chipset.map(memory);
        runtime.set_hardware(&chipset);
    }

    void poke(u32 offset, u16 value) { memory.write16(kCustomBase + offset, value); }
    [[nodiscard]] u16 peek(u32 offset) { return memory.read16(kCustomBase + offset); }

    // Advance the hardware by whole colour clocks.
    void run_colour_clocks(u32 count) {
        chipset.run_until(count * kCpuCyclesPerColourClock +
                          chipset.frames_completed() * 0);
    }
    void run_frames(u32 count) {
        const u64 target = now_ + static_cast<u64>(count) * kLinesPerFramePal *
                                      kColourClocksPerLinePal * kCpuCyclesPerColourClock;
        chipset.run_until(target);
        now_ = target;
    }
    void run_lines(u32 count) {
        const u64 target = now_ + static_cast<u64>(count) * kColourClocksPerLinePal *
                                      kCpuCyclesPerColourClock;
        chipset.run_until(target);
        now_ = target;
    }

    u64 now_ = 0;

    // A standard low-resolution screen: one bitplane, 320x200.
    void setup_lores_screen(u32 planes = 1) {
        poke(reg::DIWSTRT, 0x2c81);
        poke(reg::DIWSTOP, 0xf4c1);
        poke(reg::DDFSTRT, 0x0038);
        poke(reg::DDFSTOP, 0x00d0);
        poke(reg::BPL1MOD, 0);
        poke(reg::BPL2MOD, 0);
        poke(reg::BPLCON0, static_cast<u16>(planes << BPLCON0_BPU_SHIFT));
        poke(reg::BPLCON1, 0);
        poke(reg::BPL1PTH, static_cast<u16>(kBitplane >> 16));
        poke(reg::BPL1PTL, static_cast<u16>(kBitplane));
        poke(reg::COLOR00, 0x0000);
        poke(reg::COLOR00 + 2, 0x0fff);
        poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_RASTER);
    }

    void fill_bitplane(u16 pattern, u32 words = 20 * 200) {
        for (u32 i = 0; i < words; ++i) memory.write16(kBitplane + i * 2, pattern);
    }
};

constexpr u32 kWhite = 0xffffffffu;
constexpr u32 kBlack = 0xff000000u;

} // namespace

// ---------------------------------------------------------------------------
// Register semantics
// ---------------------------------------------------------------------------

TEST_CASE("DMACON uses set and clear semantics, not assignment") {
    Machine m;
    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_RASTER | DMAF_COPPER);
    CHECK_EQ(m.peek(reg::DMACONR) & DMAF_ALL, DMAF_MASTER | DMAF_RASTER | DMAF_COPPER);

    // Clearing one bit must leave the others alone. Assignment would wipe them.
    m.poke(reg::DMACON, DMAF_COPPER);
    CHECK_EQ(m.peek(reg::DMACONR) & DMAF_ALL, DMAF_MASTER | DMAF_RASTER);

    m.poke(reg::DMACON, 0x8000 | DMAF_SPRITE);
    CHECK_EQ(m.peek(reg::DMACONR) & DMAF_ALL,
             DMAF_MASTER | DMAF_RASTER | DMAF_SPRITE);
}

TEST_CASE("INTENA and INTREQ use set and clear semantics") {
    Machine m;
    m.poke(reg::INTENA, 0x8000 | INTF_INTEN | INTF_VERTB | INTF_BLIT);
    CHECK_EQ(m.peek(reg::INTENAR), INTF_INTEN | INTF_VERTB | INTF_BLIT);

    m.poke(reg::INTENA, INTF_BLIT);
    CHECK_EQ(m.peek(reg::INTENAR), INTF_INTEN | INTF_VERTB);

    m.poke(reg::INTREQ, 0x8000 | INTF_VERTB);
    CHECK_EQ(m.peek(reg::INTREQR) & INTF_VERTB, INTF_VERTB);
    m.poke(reg::INTREQ, INTF_VERTB);
    CHECK_EQ(m.peek(reg::INTREQR) & INTF_VERTB, 0u);
}

TEST_CASE("custom registers are not storage: a write is not read back") {
    Machine m;
    // DMACONR reflects DMACON plus live status, and INTREQR is not a latch
    // the guest can write arbitrary values into.
    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER);
    CHECK((m.peek(reg::DMACONR) & 0x8000u) == 0u);   // bit 15 is not a DMA bit
}

TEST_CASE("the beam counters advance and are readable") {
    Machine m;
    m.run_lines(3);
    CHECK_EQ(m.chipset.vpos(), 3u);
    const u16 vhpos = m.peek(reg::VHPOSR);
    CHECK_EQ((vhpos >> 8) & 0xff, 3u);
}

TEST_CASE("the vertical blank interrupt is raised once per frame") {
    Machine m;
    m.poke(reg::INTENA, 0x8000 | INTF_INTEN | INTF_VERTB);
    m.poke(reg::INTREQ, INTF_VERTB);       // clear any pending request

    m.run_lines(10);
    CHECK_EQ(m.peek(reg::INTREQR) & INTF_VERTB, 0u);

    m.run_frames(1);
    CHECK_EQ(m.peek(reg::INTREQR) & INTF_VERTB, INTF_VERTB);
    // VERTB is a level 3 interrupt.
    CHECK_EQ(m.chipset.pending_interrupt_level(), 3u);
}

TEST_CASE("an interrupt is not pending while the master enable is clear") {
    Machine m;
    m.poke(reg::INTENA, 0x8000 | INTF_VERTB);   // no INTEN
    m.run_frames(1);
    CHECK_EQ(m.chipset.pending_interrupt_level(), 0u);

    m.poke(reg::INTENA, 0x8000 | INTF_INTEN);
    CHECK_EQ(m.chipset.pending_interrupt_level(), 3u);
}

TEST_CASE("the interrupt level is the highest requested and enabled") {
    Machine m;
    m.poke(reg::INTENA, 0x8000 | INTF_INTEN | INTF_VERTB | INTF_EXTER | INTF_SOFT);
    m.poke(reg::INTREQ, 0x7fff);   // clear everything the reset frame raised
    m.poke(reg::INTREQ, 0x8000 | INTF_SOFT);
    CHECK_EQ(m.chipset.pending_interrupt_level(), 1u);
    m.poke(reg::INTREQ, 0x8000 | INTF_VERTB);
    CHECK_EQ(m.chipset.pending_interrupt_level(), 3u);
    m.poke(reg::INTREQ, 0x8000 | INTF_EXTER);
    CHECK_EQ(m.chipset.pending_interrupt_level(), 6u);
    CHECK_EQ(m.chipset.acknowledge_interrupt(6), 30u);   // autovector 24 + level
}

// ---------------------------------------------------------------------------
// Copper
// ---------------------------------------------------------------------------

TEST_CASE("the copper executes MOVE instructions from chip ram") {
    Machine m;
    u32 p = kCopperList;
    auto emit = [&](u16 a, u16 b) {
        m.memory.write16(p, a);
        m.memory.write16(p + 2, b);
        p += 4;
    };
    emit(reg::COLOR00, 0x0123);            // move #$0123,color00
    emit(reg::COLOR00 + 2, 0x0456);        // move #$0456,color01
    emit(0xffff, 0xfffe);                  // end of list

    m.poke(reg::COP1LCH, static_cast<u16>(kCopperList >> 16));
    m.poke(reg::COP1LCL, static_cast<u16>(kCopperList));
    m.poke(reg::COPJMP1, 0);
    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_COPPER);

    m.run_lines(1);
    CHECK_EQ(m.chipset.peek_register(reg::COLOR00), 0x0123u);
    CHECK_EQ(m.chipset.peek_register(reg::COLOR00 + 2), 0x0456u);
}

TEST_CASE("the copper does not run without its dma channel") {
    Machine m;
    m.memory.write16(kCopperList, reg::COLOR00);
    m.memory.write16(kCopperList + 2, 0x0fff);
    m.memory.write16(kCopperList + 4, 0xffff);
    m.memory.write16(kCopperList + 6, 0xfffe);

    m.poke(reg::COP1LCH, static_cast<u16>(kCopperList >> 16));
    m.poke(reg::COP1LCL, static_cast<u16>(kCopperList));
    m.poke(reg::COPJMP1, 0);
    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER);   // no COPPER bit

    m.run_lines(2);
    CHECK_EQ(m.chipset.peek_register(reg::COLOR00), 0x0000u);
}

TEST_CASE("a copper WAIT holds until the beam reaches the line") {
    Machine m;
    u32 p = kCopperList;
    auto emit = [&](u16 a, u16 b) {
        m.memory.write16(p, a);
        m.memory.write16(p + 2, b);
        p += 4;
    };
    // Wait for line 100, then change the background colour.
    emit(0x6401, 0xfffe);
    emit(reg::COLOR00, 0x0f00);
    emit(0xffff, 0xfffe);

    m.poke(reg::COP1LCH, static_cast<u16>(kCopperList >> 16));
    m.poke(reg::COP1LCL, static_cast<u16>(kCopperList));
    m.poke(reg::COPJMP1, 0);
    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_COPPER);

    m.run_lines(50);
    CHECK_EQ(m.chipset.peek_register(reg::COLOR00), 0x0000u);   // still waiting

    m.run_lines(51);
    CHECK_EQ(m.chipset.peek_register(reg::COLOR00), 0x0f00u);
}

TEST_CASE("copper bars: a colour change per line produces a gradient") {
    // The spec's copper-bars criterion (section 64.3). One WAIT and one MOVE
    // per line, and the resulting raster must show a different colour on each.
    Machine m;
    m.setup_lores_screen(0);      // no bitplanes: the background is the picture

    u32 p = kCopperList;
    auto emit = [&](u16 a, u16 b) {
        m.memory.write16(p, a);
        m.memory.write16(p + 2, b);
        p += 4;
    };
    for (u32 line = 0; line < 8; ++line) {
        const u32 y = 50 + line;
        emit(static_cast<u16>((y << 8) | 0x01), 0xfffe);
        emit(reg::COLOR00, static_cast<u16>(0x0100 * (line + 1)));
    }
    emit(0xffff, 0xfffe);

    m.poke(reg::COP1LCH, static_cast<u16>(kCopperList >> 16));
    m.poke(reg::COP1LCL, static_cast<u16>(kCopperList));
    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_COPPER | DMAF_RASTER);

    m.run_frames(2);
    const Framebuffer& fb = m.chipset.framebuffer();
    CHECK(fb.width > 0);

    // The view starts at the top of the display window, line 44.
    for (u32 line = 0; line < 8; ++line) {
        const u32 y = 50 + line - 44;
        if (y >= fb.height) continue;
        const u32 expected_red = ((line + 1) & 0x0f) * 17u;
        const u32 pixel = fb.pixels[static_cast<std::size_t>(y) * fb.width + fb.width / 2];
        CHECK_EQ((pixel >> 16) & 0xff, expected_red);
    }
}

// ---------------------------------------------------------------------------
// Bitplanes
// ---------------------------------------------------------------------------

TEST_CASE("a one bitplane display shows the expected image") {
    // The spec's bitplane criterion (section 64.4).
    Machine m;
    m.setup_lores_screen(1);
    m.fill_bitplane(0xaaaa);    // alternating pixels, most significant first
    m.run_frames(2);

    const Framebuffer& fb = m.chipset.framebuffer();
    CHECK_EQ(fb.width, 640u);    // 320 lores pixels, two framebuffer columns each
    CHECK_EQ(fb.height, 200u);

    const u32* row = fb.pixels.data() + static_cast<std::size_t>(100) * fb.width;
    // Lores pixel 0 is set, pixel 1 is clear, and each covers two columns.
    CHECK_EQ(row[0], kWhite);
    CHECK_EQ(row[1], kWhite);
    CHECK_EQ(row[2], kBlack);
    CHECK_EQ(row[3], kBlack);
    CHECK_EQ(row[4], kWhite);
}

TEST_CASE("two bitplanes select four colours") {
    Machine m;
    m.setup_lores_screen(2);
    m.poke(reg::COLOR00 + 4, 0x00f0);
    m.poke(reg::COLOR00 + 6, 0x000f);
    // Plane 1 is the low bit, plane 2 the high bit. Interleave them so the
    // first four pixels are 0, 1, 2, 3.
    for (u32 row = 0; row < 200; ++row) {
        m.memory.write16(kBitplane + row * 40, 0x5000);          // plane 1: 0101
        m.memory.write16(kBitplane + row * 40 + 2, 0x3000);      // plane 2: 0011
    }
    // The second plane's data follows the first, one word per line each.
    m.poke(reg::BPL1PTH, static_cast<u16>(kBitplane >> 16));
    m.poke(reg::BPL1PTL, static_cast<u16>(kBitplane));
    m.poke(reg::BPL1PTH + 4, static_cast<u16>((kBitplane + 2) >> 16));
    m.poke(reg::BPL1PTL + 4, static_cast<u16>(kBitplane + 2));
    m.run_frames(2);

    const Framebuffer& fb = m.chipset.framebuffer();
    const u32* row = fb.pixels.data() + static_cast<std::size_t>(10) * fb.width;
    // Pixels: plane1 0,1,0,1 and plane2 0,0,1,1 give indices 0,1,2,3.
    CHECK_EQ(row[0], kBlack);                     // colour 0
    CHECK_EQ(row[2], kWhite);                     // colour 1
    CHECK_EQ(row[4] & 0x0000ff00u, 0x0000ff00u);  // colour 2 is green
    CHECK_EQ(row[6] & 0x000000ffu, 0x000000ffu);  // colour 3 is blue
}

TEST_CASE("bplcon1 scroll delays the playfield") {
    Machine m;
    m.setup_lores_screen(1);
    m.fill_bitplane(0x8000);       // one set pixel at the start of each word
    m.poke(reg::BPLCON1, 0x0004);  // four lores pixels of delay
    m.run_frames(2);

    const Framebuffer& fb = m.chipset.framebuffer();
    const u32* row = fb.pixels.data() + static_cast<std::size_t>(50) * fb.width;
    // The set pixel has moved four lores pixels to the right, so eight columns.
    CHECK_EQ(row[0], kBlack);
    CHECK_EQ(row[8], kWhite);
}

TEST_CASE("the bitplane modulo advances the pointer between lines") {
    Machine m;
    m.setup_lores_screen(1);
    // A bitmap 44 bytes wide displayed 40 bytes wide: modulo 4.
    m.poke(reg::BPL1MOD, 4);
    for (u32 row = 0; row < 200; ++row) {
        for (u32 word = 0; word < 22; ++word)
            m.memory.write16(kBitplane + row * 44 + word * 2,
                             (row & 1) ? 0xffff : 0x0000);
    }
    m.run_frames(2);

    const Framebuffer& fb = m.chipset.framebuffer();
    // Even rows are clear, odd rows set. If the modulo were ignored the rows
    // would drift instead.
    CHECK_EQ(fb.pixels[static_cast<std::size_t>(0) * fb.width + 4], kBlack);
    CHECK_EQ(fb.pixels[static_cast<std::size_t>(1) * fb.width + 4], kWhite);
    CHECK_EQ(fb.pixels[static_cast<std::size_t>(20) * fb.width + 4], kBlack);
    CHECK_EQ(fb.pixels[static_cast<std::size_t>(21) * fb.width + 4], kWhite);
}

// ---------------------------------------------------------------------------
// Blitter
// ---------------------------------------------------------------------------

namespace {

void run_blit_to_completion(Machine& m) {
    // Blits are asynchronous; give the hardware time and check it finished.
    for (int i = 0; i < 200 && m.chipset.blitter_busy(); ++i) m.run_lines(1);
}

} // namespace

TEST_CASE("a simple blit copies chip ram and raises its interrupt") {
    // The spec's blitter criterion (section 64.5).
    Machine m;
    const u32 source = 0x00040000;
    const u32 dest = 0x00050000;
    for (u32 i = 0; i < 16; ++i) m.memory.write16(source + i * 2, static_cast<u16>(0x1000 + i));

    m.poke(reg::INTENA, 0x8000 | INTF_INTEN | INTF_BLIT);
    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_BLITTER);

    m.poke(reg::BLTCON0, 0x09f0);   // USEA | USED, minterm D = A
    m.poke(reg::BLTCON1, 0x0000);
    m.poke(reg::BLTAFWM, 0xffff);
    m.poke(reg::BLTALWM, 0xffff);
    m.poke(reg::BLTAMOD, 0);
    m.poke(reg::BLTDMOD, 0);
    m.poke(reg::BLTAPTH, static_cast<u16>(source >> 16));
    m.poke(reg::BLTAPTL, static_cast<u16>(source));
    m.poke(reg::BLTDPTH, static_cast<u16>(dest >> 16));
    m.poke(reg::BLTDPTL, static_cast<u16>(dest));

    CHECK(!m.chipset.blitter_busy());
    m.poke(reg::BLTSIZE, (4 << 6) | 4);   // four rows of four words
    CHECK(m.chipset.blitter_busy());
    CHECK_EQ(m.peek(reg::DMACONR) & 0x4000u, 0x4000u);   // BBUSY

    run_blit_to_completion(m);
    CHECK(!m.chipset.blitter_busy());

    for (u32 i = 0; i < 16; ++i)
        CHECK_EQ(m.memory.read16(dest + i * 2), 0x1000u + i);

    CHECK_EQ(m.peek(reg::INTREQR) & INTF_BLIT, INTF_BLIT);
}

TEST_CASE("the blitter applies its minterm") {
    Machine m;
    const u32 a = 0x00040000, c = 0x00041000, d = 0x00050000;
    m.memory.write16(a, 0xff00);
    m.memory.write16(c, 0x0ff0);

    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_BLITTER);
    // USEA | USEC | USED with minterm $fa, which is A OR C for any B.
    m.poke(reg::BLTCON0, 0x0b00 | 0xfa);
    m.poke(reg::BLTCON1, 0);
    m.poke(reg::BLTAFWM, 0xffff);
    m.poke(reg::BLTALWM, 0xffff);
    m.poke(reg::BLTAMOD, 0);
    m.poke(reg::BLTCMOD, 0);
    m.poke(reg::BLTDMOD, 0);
    m.poke(reg::BLTAPTH, static_cast<u16>(a >> 16));
    m.poke(reg::BLTAPTL, static_cast<u16>(a));
    m.poke(reg::BLTCPTH, static_cast<u16>(c >> 16));
    m.poke(reg::BLTCPTL, static_cast<u16>(c));
    m.poke(reg::BLTDPTH, static_cast<u16>(d >> 16));
    m.poke(reg::BLTDPTL, static_cast<u16>(d));
    m.poke(reg::BLTSIZE, (1 << 6) | 1);
    run_blit_to_completion(m);

    CHECK_EQ(m.memory.read16(d), 0xfff0u);
}

TEST_CASE("the blitter masks the first and last word of each row") {
    Machine m;
    const u32 a = 0x00040000, d = 0x00050000;
    m.memory.write16(a, 0xffff);
    m.memory.write16(a + 2, 0xffff);

    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_BLITTER);
    m.poke(reg::BLTCON0, 0x09f0);
    m.poke(reg::BLTCON1, 0);
    m.poke(reg::BLTAFWM, 0x00ff);
    m.poke(reg::BLTALWM, 0xff00);
    m.poke(reg::BLTAMOD, 0);
    m.poke(reg::BLTDMOD, 0);
    m.poke(reg::BLTAPTH, static_cast<u16>(a >> 16));
    m.poke(reg::BLTAPTL, static_cast<u16>(a));
    m.poke(reg::BLTDPTH, static_cast<u16>(d >> 16));
    m.poke(reg::BLTDPTL, static_cast<u16>(d));
    m.poke(reg::BLTSIZE, (1 << 6) | 2);
    run_blit_to_completion(m);

    CHECK_EQ(m.memory.read16(d), 0x00ffu);
    CHECK_EQ(m.memory.read16(d + 2), 0xff00u);
}

TEST_CASE("the blitter shifts the a channel across word boundaries") {
    Machine m;
    const u32 a = 0x00040000, d = 0x00050000;
    m.memory.write16(a, 0x000f);
    m.memory.write16(a + 2, 0x0000);

    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_BLITTER);
    m.poke(reg::BLTCON0, 0x4000 | 0x09f0);   // shift A right by four
    m.poke(reg::BLTCON1, 0);
    m.poke(reg::BLTAFWM, 0xffff);
    m.poke(reg::BLTALWM, 0xffff);
    m.poke(reg::BLTAMOD, 0);
    m.poke(reg::BLTDMOD, 0);
    m.poke(reg::BLTAPTH, static_cast<u16>(a >> 16));
    m.poke(reg::BLTAPTL, static_cast<u16>(a));
    m.poke(reg::BLTDPTH, static_cast<u16>(d >> 16));
    m.poke(reg::BLTDPTL, static_cast<u16>(d));
    m.poke(reg::BLTSIZE, (1 << 6) | 2);
    run_blit_to_completion(m);

    CHECK_EQ(m.memory.read16(d), 0x0000u);       // $000f shifted right by 4
    CHECK_EQ(m.memory.read16(d + 2), 0xf000u);   // the bits land in the next word
}

TEST_CASE("the blitter applies modulos between rows") {
    Machine m;
    const u32 a = 0x00040000, d = 0x00050000;
    for (u32 i = 0; i < 8; ++i) m.memory.write16(a + i * 2, static_cast<u16>(i + 1));

    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_BLITTER);
    m.poke(reg::BLTCON0, 0x09f0);
    m.poke(reg::BLTCON1, 0);
    m.poke(reg::BLTAFWM, 0xffff);
    m.poke(reg::BLTALWM, 0xffff);
    m.poke(reg::BLTAMOD, 2);    // skip a word between source rows
    m.poke(reg::BLTDMOD, 0);
    m.poke(reg::BLTAPTH, static_cast<u16>(a >> 16));
    m.poke(reg::BLTAPTL, static_cast<u16>(a));
    m.poke(reg::BLTDPTH, static_cast<u16>(d >> 16));
    m.poke(reg::BLTDPTL, static_cast<u16>(d));
    m.poke(reg::BLTSIZE, (2 << 6) | 2);   // two rows of two words
    run_blit_to_completion(m);

    CHECK_EQ(m.memory.read16(d), 1u);
    CHECK_EQ(m.memory.read16(d + 2), 2u);
    CHECK_EQ(m.memory.read16(d + 4), 4u);   // row two skipped word three
    CHECK_EQ(m.memory.read16(d + 6), 5u);
}

TEST_CASE("inclusive fill spans between set bits") {
    Machine m;
    const u32 a = 0x00040000, d = 0x00050000;
    m.memory.write16(a, 0x0102);   // bits 1 and 8 set

    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_BLITTER);
    m.poke(reg::BLTCON0, 0x09f0);
    m.poke(reg::BLTCON1, 0x0008);  // inclusive fill
    m.poke(reg::BLTAFWM, 0xffff);
    m.poke(reg::BLTALWM, 0xffff);
    m.poke(reg::BLTAMOD, 0);
    m.poke(reg::BLTDMOD, 0);
    m.poke(reg::BLTAPTH, static_cast<u16>(a >> 16));
    m.poke(reg::BLTAPTL, static_cast<u16>(a));
    m.poke(reg::BLTDPTH, static_cast<u16>(d >> 16));
    m.poke(reg::BLTDPTL, static_cast<u16>(d));
    m.poke(reg::BLTSIZE, (1 << 6) | 1);
    run_blit_to_completion(m);

    // Bits 1 through 8 inclusive.
    CHECK_EQ(m.memory.read16(d), 0x01feu);
}

TEST_CASE("a descending blit walks backwards through memory") {
    Machine m;
    const u32 a = 0x00040000, d = 0x00050000;
    for (u32 i = 0; i < 4; ++i) m.memory.write16(a + i * 2, static_cast<u16>(i + 1));

    m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_BLITTER);
    m.poke(reg::BLTCON0, 0x09f0);
    m.poke(reg::BLTCON1, 0x0002);   // descending
    m.poke(reg::BLTAFWM, 0xffff);
    m.poke(reg::BLTALWM, 0xffff);
    m.poke(reg::BLTAMOD, 0);
    m.poke(reg::BLTDMOD, 0);
    // A descending blit is given the address of the last word.
    m.poke(reg::BLTAPTH, static_cast<u16>((a + 6) >> 16));
    m.poke(reg::BLTAPTL, static_cast<u16>(a + 6));
    m.poke(reg::BLTDPTH, static_cast<u16>((d + 6) >> 16));
    m.poke(reg::BLTDPTL, static_cast<u16>(d + 6));
    m.poke(reg::BLTSIZE, (1 << 6) | 4);
    run_blit_to_completion(m);

    for (u32 i = 0; i < 4; ++i)
        CHECK_EQ(m.memory.read16(d + i * 2), i + 1u);
}

// ---------------------------------------------------------------------------
// CIA
// ---------------------------------------------------------------------------

TEST_CASE("a cia timer counts down and raises its interrupt") {
    Machine m;
    m.poke(reg::INTENA, 0x8000 | INTF_INTEN | INTF_PORTS);

    // CIA-A timer A: load 100, enable its interrupt, start it.
    m.memory.write8(0x00bfe401, 100);       // TALO
    m.memory.write8(0x00bfe501, 0);         // TAHI
    m.memory.write8(0x00bfed01, 0x80 | 0x01);   // ICR: enable timer A
    m.memory.write8(0x00bfee01, 0x01);      // CRA: start

    // The CIA counts at a fifth of the colour clock, so 100 ticks is a little
    // over two scan lines.
    m.run_lines(5);
    CHECK_EQ(m.peek(reg::INTREQR) & INTF_PORTS, INTF_PORTS);
}

TEST_CASE("the fire buttons appear in cia-a port a, active low") {
    Machine m;
    m.chipset.set_joystick(0, false, false, false, false, true);
    const u8 pra = m.memory.read8(0x00bfe001);
    CHECK_EQ(pra & 0x40u, 0u);          // port 0 fire pulls its bit low
    CHECK_EQ(pra & 0x80u, 0x80u);       // port 1 is not pressed
}

// A digital joystick presents itself through two quadrature counters, and a
// game decodes it as right = bit 1, down = bit 1 xor bit 0, left = bit 9,
// up = bit 9 xor bit 8. Encoding it any other way makes a press for one
// direction read as another, which is invisible until a real game ignores you.
namespace {

struct Direction {
    bool up, down, left, right;
};

// The decode every game performs, written out here independently.
Direction decode_joydat(u16 value) {
    const bool bit0 = (value & 0x0001) != 0;
    const bool bit1 = (value & 0x0002) != 0;
    const bool bit8 = (value & 0x0100) != 0;
    const bool bit9 = (value & 0x0200) != 0;
    return {bit9 != bit8, bit1 != bit0, bit9, bit1};
}

} // namespace

TEST_CASE("every joystick direction decodes back to itself") {
    Machine m;
    struct Case { bool up, down, left, right; const char* name; };
    const Case cases[] = {
        {false, false, false, false, "neutral"},
        {true,  false, false, false, "up"},
        {false, true,  false, false, "down"},
        {false, false, true,  false, "left"},
        {false, false, false, true,  "right"},
        {true,  false, true,  false, "up-left"},
        {true,  false, false, true,  "up-right"},
        {false, true,  true,  false, "down-left"},
        {false, true,  false, true,  "down-right"},
    };
    for (const Case& c : cases) {
        m.chipset.set_joystick(1, c.up, c.down, c.left, c.right, false);
        const Direction got = decode_joydat(m.peek(reg::JOY1DAT));
        if (got.up != c.up || got.down != c.down || got.left != c.left ||
            got.right != c.right) {
            report_failure(__FILE__, __LINE__,
                           std::string("joystick ") + c.name + " decoded wrong");
        }
    }
}

TEST_CASE("the mouse is a pair of counters, not a position") {
    Machine m;
    // A game reads JOY0DAT every frame and takes the difference, so movement
    // has to accumulate rather than replace.
    m.chipset.set_mouse(3, 5, false, false);
    CHECK_EQ(m.peek(reg::JOY0DAT) & 0xffu, 3u);
    CHECK_EQ((m.peek(reg::JOY0DAT) >> 8) & 0xffu, 5u);

    m.chipset.set_mouse(4, 2, false, false);
    CHECK_EQ(m.peek(reg::JOY0DAT) & 0xffu, 7u);
    CHECK_EQ((m.peek(reg::JOY0DAT) >> 8) & 0xffu, 7u);

    // And the counters wrap, which is what the delta arithmetic expects.
    m.chipset.set_mouse(-8, 0, false, false);
    CHECK_EQ(m.peek(reg::JOY0DAT) & 0xffu, 0xffu);
}

TEST_CASE("mouse buttons land where a game looks for them") {
    Machine m;
    m.chipset.set_mouse(0, 0, true, false);
    CHECK_EQ(m.memory.read8(0x00bfe001) & 0x40u, 0u);       // left: CIA-A PRA bit 6
    m.chipset.set_mouse(0, 0, false, true);
    CHECK_EQ(m.memory.read8(0x00bfe001) & 0x40u, 0x40u);
    CHECK_EQ(m.peek(reg::POTGOR) & 0x0400u, 0u);        // right: POTGOR bit 10
}

// ---------------------------------------------------------------------------
// Machine state
// ---------------------------------------------------------------------------

TEST_CASE("chipset state survives a save and restore") {
    // A snapshot is memory and machine state. Restoring one without the other
    // leaves the Copper pointing at address zero and DMA disabled, which is a
    // machine no game ever configured (AMIGA_RECOMP.md 39).
    const std::string path = std::string(ARECOMP_TESTROM_DIR) + "/../../chipset.state";

    u32 saved_copper_source = 0;
    {
        Machine m;
        m.setup_lores_screen(2);
        m.poke(reg::COP1LCH, 0x0003);
        m.poke(reg::COP1LCL, 0x0000);
        m.poke(reg::COPJMP1, 0);
        m.poke(reg::INTENA, 0x8000 | INTF_INTEN | INTF_COPER | INTF_VERTB);
        m.poke(reg::DMACON, 0x8000 | DMAF_MASTER | DMAF_RASTER | DMAF_COPPER);
        m.poke(reg::COLOR00 + 8, 0x0abc);
        m.run_lines(5);
        saved_copper_source = m.chipset.peek_register(reg::COP1LCL);
        CHECK(m.chipset.save_state(path));
    }

    {
        Machine fresh;
        // A fresh chipset has none of it.
        CHECK_EQ(fresh.chipset.peek_register(reg::INTENA), 0u);
        CHECK_EQ(fresh.chipset.peek_register(reg::DMACON) & DMAF_MASTER, 0u);

        CHECK(fresh.chipset.load_state(path));
        CHECK_EQ(fresh.chipset.peek_register(reg::INTENA),
                 INTF_INTEN | INTF_COPER | INTF_VERTB);
        CHECK_EQ(fresh.chipset.peek_register(reg::DMACON) & DMAF_ALL,
                 DMAF_MASTER | DMAF_RASTER | DMAF_COPPER);
        CHECK_EQ(fresh.chipset.peek_register(reg::COP1LCL), saved_copper_source);
        CHECK_EQ(fresh.chipset.peek_register(reg::COLOR00 + 8), 0x0abcu);
        CHECK_EQ(fresh.chipset.peek_register(reg::BPLCON0) >> BPLCON0_BPU_SHIFT, 2u);
    }
    std::remove(path.c_str());
}

TEST_CASE("loading rubbish as chipset state is refused, not half applied") {
    Machine m;
    CHECK(!m.chipset.load_state("no-such-file-at-all.state"));
    CHECK_EQ(m.chipset.peek_register(reg::INTENA), 0u);
}

ARECOMP_TEST_MAIN()
