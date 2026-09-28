// Self-modifying and overwritten code (AMIGA_RECOMP.md 32).
//
// A translated block describes the bytes that were there when arecomp ran. If
// the guest writes over them -- a depacker unpacking into its own space, a
// loader dropping an overlay on top of old code -- the translation no longer
// describes reality. Continuing to run it would execute code the game has
// replaced, which is the worst kind of wrong: silent and plausible.
//
// So pages holding translated code are watched, and a write to one drops the
// blocks on it. Execution there falls back to the interpreter, which reads
// memory as it now is.

#include "machine_fixture.hpp"
#include "test_support.hpp"

using namespace arecomp;
using namespace arecomp::test;

namespace {

// Stand-in translated blocks. What they do does not matter; what matters is
// whether the dispatcher still offers them after the guest writes.
u32 block_a(M68kState& cpu, Runtime&) {
    cpu.d[0] = 0xaaaaaaaau;
    return 0x2000;
}
u32 block_b(M68kState& cpu, Runtime&) {
    cpu.d[1] = 0xbbbbbbbbu;
    return 0x3000;
}
// On a different page from the first two.
u32 block_far(M68kState& cpu, Runtime&) {
    cpu.d[2] = 0xccccccccu;
    return 0x4000;
}

const BlockEntry kBlocks[] = {
    {0x00001000, &block_a},
    {0x00001100, &block_b},
    {0x00008000, &block_far},
};

} // namespace

TEST_CASE("a translated block is used until its code is overwritten") {
    Machine m;
    m.runtime.blocks().build(kBlocks, 3);
    m.runtime.watch_translated_code();

    CHECK(m.runtime.blocks().find(0x00001000) != nullptr);
    CHECK(m.runtime.blocks().find(0x00001100) != nullptr);

    // A write to the page those blocks came from retires them.
    m.memory.write16(0x00001080, 0x4e71);

    CHECK(m.runtime.blocks().find(0x00001000) == nullptr);
    CHECK(m.runtime.blocks().find(0x00001100) == nullptr);
    CHECK_EQ(m.runtime.stats().code_overwrites, 1u);
    CHECK_EQ(m.runtime.invalidated_blocks(), 2u);
}

TEST_CASE("blocks on other pages are left alone") {
    Machine m;
    m.runtime.blocks().build(kBlocks, 3);
    m.runtime.watch_translated_code();

    m.memory.write16(0x00001080, 0x4e71);
    // $8000 is a different page and was not written.
    CHECK(m.runtime.blocks().find(0x00008000) != nullptr);
    CHECK_EQ(m.runtime.invalidated_blocks(), 2u);
}

TEST_CASE("writing somewhere with no translated code costs nothing") {
    Machine m;
    m.runtime.blocks().build(kBlocks, 3);
    m.runtime.watch_translated_code();

    m.memory.write32(0x00020000, 0x12345678);
    CHECK_EQ(m.runtime.stats().code_overwrites, 0u);
    CHECK(m.runtime.blocks().find(0x00001000) != nullptr);
}

TEST_CASE("a page is reported once, however many times it is written") {
    Machine m;
    m.runtime.blocks().build(kBlocks, 3);
    m.runtime.watch_translated_code();

    for (int i = 0; i < 50; ++i) m.memory.write8(0x00001200 + i, 0xff);
    CHECK_EQ(m.runtime.stats().code_overwrites, 1u);
}

TEST_CASE("execution falls back to the interpreter after an overwrite") {
    Machine m;
    // A real instruction at $1000 for the interpreter to run: moveq #7,d0.
    m.load_words({0x7007}, 0x1000);
    m.runtime.blocks().build(kBlocks, 3);
    m.runtime.watch_translated_code();

    // Before the overwrite the compiled block runs and sets its marker.
    m.cpu.pc = 0x1000;
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.d[0], 0xaaaaaaaau);

    // The guest rewrites that page; now the real instruction runs instead.
    m.memory.write16(0x00001400, 0x4e71);
    m.cpu.d[0] = 0;
    m.cpu.pc = 0x1000;
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.d[0], 7u);          // moveq #7,d0, interpreted
    CHECK_EQ(m.cpu.pc, 0x1002u);
    CHECK(m.runtime.stats().interpreter_fallbacks > 0);
}

TEST_CASE("rom pages are never reported: a write there does nothing") {
    Machine m;
    std::vector<u8> contents(GuestMemory::kPageSize, 0x4e);
    m.memory.add_rom(0x00f80000, contents, "rom");
    const BlockEntry rom_block[] = {{0x00f80000, &block_a}};
    m.runtime.blocks().build(rom_block, 1);
    m.runtime.watch_translated_code();

    m.memory.write16(0x00f80010, 0x0000);   // ignored by a read-only region
    CHECK_EQ(m.runtime.stats().code_overwrites, 0u);
    CHECK(m.runtime.blocks().find(0x00f80000) != nullptr);
}

ARECOMP_TEST_MAIN()
