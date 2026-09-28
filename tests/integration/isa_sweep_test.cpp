// Absolute expectations for the ISA sweep program.
//
// The differential test proves that generated code matches the interpreter.
// It cannot prove that either is *right*, because both call the same semantic
// helpers. This file closes that gap: every value below is derived by hand
// from the MC68000 User's Manual, so the pair can be wrong together only if
// these expectations are also wrong.
#include "machine_fixture.hpp"
#include "test_support.hpp"

using namespace arecomp;
using namespace arecomp::test;

namespace {

constexpr u32 kWork = 0x3000;

struct Sweep {
    Machine m;

    Sweep() {
        const bool loaded = m.load_file(testrom_path("isa_sweep.bin"));
        if (!loaded) report_failure(__FILE__, __LINE__, "isa_sweep.bin not found");
        m.run();
        if (!m.cpu.halted)
            report_failure(__FILE__, __LINE__, "isa_sweep did not reach its sentinel");
    }

    [[nodiscard]] u32 l(u32 offset) const { return m.memory.peek32(kWork + offset); }
    [[nodiscard]] u16 w(u32 offset) const { return m.memory.peek16(kWork + offset); }
    [[nodiscard]] u8 b(u32 offset) const { return m.memory.peek8(kWork + offset); }
};

} // namespace

TEST_CASE("every addressing mode reaches the right operand") {
    const Sweep s;
    CHECK_EQ(s.l(0), 0x0f0f0f0fu);    // (a0)
    CHECK_EQ(s.l(4), 0x12345678u);    // d16(a0)
    CHECK_EQ(s.l(8), 0x12345678u);    // d8(a0,d2.w)
    CHECK_EQ(s.l(12), 0x12345678u);   // d8(a0,d2.l)
    CHECK_EQ(s.l(16), 0x0f0f0f0fu);   // absolute long
    CHECK_EQ(s.l(20), 0x0f0f0f0fu);   // (a2)+
    CHECK_EQ(s.l(24), 0x0f0f0f0fu);   // -(a2)
    CHECK_EQ(s.l(28), 0x00002000u);   // the pointer is back where it started
    CHECK_EQ(s.w(32), 0xc0deu);       // d16(pc)
}

TEST_CASE("arithmetic produces the documented results") {
    const Sweep s;
    CHECK_EQ(s.w(40), 0xffffu);       // $8000 + $7fff
    CHECK_EQ(s.w(44), 0x8000u);       // $7fff + 1
    CHECK_EQ(s.w(48), 0x00ffu);       // 0 - 1, byte
    CHECK_EQ(s.l(52), 0x0000ffffu);   // $10000 - 1
    CHECK_EQ(s.l(56), 0xaaaa0000u);   // and
    CHECK_EQ(s.l(60), 0xaaaaffffu);   // or
    CHECK_EQ(s.l(64), 0x5555aaaau);   // eor
    CHECK_EQ(s.l(68), 0xaaaa5555u);   // not
    CHECK_EQ(s.l(72), 0x00000080u);   // neg.b $80 is $80
    CHECK_EQ(s.l(76), 0xffffffffu);   // neg.l 1
}

TEST_CASE("flags after arithmetic match the manual") {
    const Sweep s;
    // $8000 + $7fff = $ffff: negative, no overflow, no carry.
    CHECK_EQ(s.w(42) & 0x1fu, 0x08u);
    // $7fff + 1 = $8000: negative and overflow, no carry.
    CHECK_EQ(s.w(46) & 0x1fu, 0x0au);
    // 0 - 1 byte: negative, carry and extend.
    CHECK_EQ(s.w(50) & 0x1fu, 0x19u);
}

TEST_CASE("addx and subx carry the extend bit between operations") {
    const Sweep s;
    CHECK_EQ(s.w(80), 0x0002u);       // $ffff + 1 sets X, then 0 + 1 + X
    CHECK_EQ(s.w(82) & 0x1fu, 0x00u); // non-zero result clears the accumulated Z
    CHECK_EQ(s.w(84), 0xfffdu);       // 0 - 1 sets X, then $ffff - 1 - X
}

TEST_CASE("bcd arithmetic works in decimal") {
    const Sweep s;
    CHECK_EQ(s.b(88), 0x42u);         // abcd 28 + 14
    CHECK_EQ(s.b(89), 0x28u);         // sbcd 42 - 14
    CHECK_EQ(s.b(90), 0x99u);         // nbcd 01
}

TEST_CASE("multiply and divide agree with the manual, including overflow") {
    const Sweep s;
    CHECK_EQ(s.l(92), 0xfffe0001u);   // mulu $ffff * $ffff
    CHECK_EQ(s.l(96), 0x00000001u);   // muls -1 * -1
    CHECK_EQ(s.l(100), 0x0002000eu);  // divu 100 / 7: remainder 2, quotient 14
    CHECK_EQ(s.l(104), 0xfffefff2u);  // divs -100 / 7: remainder -2, quotient -14
    CHECK_EQ(s.l(108), 0x00010000u);  // overflow leaves the destination alone
    CHECK_EQ(s.w(112) & 0x02u, 0x02u);// and sets V
}

TEST_CASE("shifts and rotates match the manual, including the edge counts") {
    const Sweep s;
    CHECK_EQ(s.w(116), 0x0080u);      // asl.b $40 -> $80
    CHECK_EQ(s.w(118) & 0x02u, 0x02u);// the sign changed, so V is set
    CHECK_EQ(s.l(120), 0xf8000000u);  // asr.l replicates the sign
    CHECK_EQ(s.w(124), 0x4000u);      // lsr.w $8001
    CHECK_EQ(s.b(126), 0x03u);        // rol.b $81
    CHECK_EQ(s.b(127), 0xc0u);        // ror.b $81
    CHECK_EQ(s.w(128), 0x0000u);      // roxl.w $8000, X was clear
    CHECK_EQ(s.w(130), 0x0001u);      // X comes back in at the bottom
    CHECK_EQ(s.w(132), 0x0000u);      // a count past the operand width
    CHECK_EQ(s.w(136), 0x2468u);      // the memory form shifts one bit
}

TEST_CASE("bit operations address the right bit in registers and memory") {
    const Sweep s;
    CHECK_EQ(s.l(140), 0x80000008u);  // bset 3 and 31: long, modulo 32
    CHECK_EQ(s.l(144), 0x80000000u);  // bclr 3
    CHECK_EQ(s.l(148), 0x00000000u);  // bchg 31
    CHECK_EQ(s.l(152), 0x00000020u);  // dynamic bit number
    CHECK_EQ(s.b(156), 0x80u);        // byte-sized on memory, modulo 8
}

TEST_CASE("ext, swap and exg") {
    const Sweep s;
    CHECK_EQ(s.l(160), 0xffffff80u);  // ext.w sign-extends the byte
    CHECK_EQ(s.l(164), 0xffffff00u);  // ext.l sign-extends the word
    CHECK_EQ(s.l(168), 0x56781234u);  // swap
    CHECK_EQ(s.l(172), 0x22222222u);  // exg swapped the registers
    CHECK_EQ(s.l(176), 0x11111111u);
}

TEST_CASE("movep transfers alternate bytes and leaves the others alone") {
    const Sweep s;
    // The bytes between the transferred ones keep their previous contents.
    CHECK_EQ(s.l(180), 0xaa68bb0fu);
    CHECK_EQ(s.l(184), 0xcc34dd78u);
    CHECK_EQ(s.l(188), 0xaabbccddu);  // and read back identically
}

TEST_CASE("scc and dbcc") {
    const Sweep s;
    CHECK_EQ(s.b(192), 0xffu);        // seq with Z set
    CHECK_EQ(s.b(193), 0x00u);        // sne with Z set
    CHECK_EQ(s.w(194), 4u);           // dbra ran four times
    CHECK_EQ(s.w(196), 0xffffu);      // and left the counter at -1
}

TEST_CASE("movem round-trips registers through the stack") {
    const Sweep s;
    CHECK_EQ(s.l(200), 0x0000ffffu);  // d0 restored
    CHECK_EQ(s.l(204), 0x12345678u);  // d3 restored
    CHECK_EQ(s.w(208), 0xffffu);      // word form writes low words only
    CHECK_EQ(s.w(210), 0x0004u);
}

TEST_CASE("subroutine calls, frames and indirect calls") {
    const Sweep s;
    CHECK_EQ(s.w(216), 21u);          // triple(7)
    CHECK_EQ(s.l(218), 0x0007effcu);  // link left the frame pointer here
    CHECK_EQ(s.l(222), 0xcafebabeu);  // a local, addressed through the frame
    CHECK_EQ(s.l(226), 0x00002000u);  // pea pushed a real guest address
    CHECK_EQ(s.w(230), 99u);          // the indirect jsr reached its target
}

TEST_CASE("the stack is balanced when the program finishes") {
    const Sweep s;
    CHECK_EQ(s.m.cpu.a[7], kStackTop);
}

ARECOMP_TEST_MAIN()
