// End-to-end interpreter behaviour, including the vertical-slice program that
// the generated native code must later reproduce exactly.
#include "machine_fixture.hpp"
#include "test_support.hpp"

using namespace arecomp;
using namespace arecomp::test;

TEST_CASE("the vertical slice program reaches the expected final state") {
    Machine m;
    CHECK(m.load_file(testrom_path("vertical_slice.bin")));
    const u64 steps = m.run();

    CHECK(m.cpu.halted);
    CHECK(steps < 200);                       // it is a short program

    CHECK_EQ(m.cpu.d[0] & 0xffff, 55u);       // 10 + 9 + ... + 1
    CHECK_EQ(m.cpu.d[3] & 0xffff, 110u);      // doubled by the subroutine
    CHECK_EQ(m.cpu.d[4], 1u);                 // the equality branch was taken
    CHECK_EQ(m.cpu.d[5], 0x12345678u);        // postincrement load
    CHECK_EQ(m.cpu.d[6] & 0xffff, 0x5678u);   // predecrement load

    CHECK_EQ(m.cpu.a[0], 0x00002000u);
    CHECK_EQ(m.cpu.a[1], 0x00002002u);
    CHECK_EQ(m.cpu.a[7], kStackTop);          // the stack is balanced again

    CHECK_EQ(m.memory.read32(0x2000), 0x12345678u);
    CHECK_EQ(m.memory.read16(0x2004), 0xbeefu);
    CHECK_EQ(m.memory.read16(0x2008), 110u);
}

TEST_CASE("bsr and rts use the real guest stack") {
    Machine m;
    m.load_words({
        0x6100, 0x0004,   // bsr.w +4  -> 0x1006
        0x4e71,           // nop
        0x33fc, 0x0001, 0x00f0, 0x0000,   // move.w #1,$f00000 (halt)
        0x4e75,           // rts  (at 0x100e, never reached as written)
    });
    // Point the subroutine at an rts so control returns to the nop.
    m.memory.write16(0x1006, 0x4e75);
    m.cpu.pc = 0x1000;

    // One step: the bsr must push a real return address.
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.pc, 0x1006u);
    CHECK_EQ(m.cpu.a[7], kStackTop - 4);
    CHECK_EQ(m.memory.read32(m.cpu.a[7]), 0x1004u);

    m.runtime.step_block(m.cpu);              // rts
    CHECK_EQ(m.cpu.pc, 0x1004u);
    CHECK_EQ(m.cpu.a[7], kStackTop);
}

TEST_CASE("byte access through a7 still moves the stack by two") {
    Machine m;
    m.load_words({0x1f00});    // move.b d0,-(a7)
    m.cpu.d[0] = 0x55;
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.a[7], kStackTop - 2);
    // The byte is written at the decremented address itself, which is even.
    // The odd byte of the reserved word is left alone.
    CHECK_EQ(m.memory.read8(kStackTop - 2), 0x55u);
}

TEST_CASE("byte access through other address registers moves by one") {
    Machine m;
    m.load_words({0x1d00});    // move.b d0,-(a6)
    m.cpu.a[6] = 0x3000;
    m.cpu.d[0] = 0x55;
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.a[6], 0x2fffu);
}

TEST_CASE("movem saves and restores registers in the right order") {
    Machine m;
    // movem.l d0-d2/a0,-(a7) then movem.l (a7)+,d3-d5/a1
    m.load_words({0x48e7, 0xe080, 0x4cdf, 0x0238});
    m.cpu.d[0] = 0x11111111;
    m.cpu.d[1] = 0x22222222;
    m.cpu.d[2] = 0x33333333;
    m.cpu.a[0] = 0x44444444;

    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.a[7], kStackTop - 16);
    // Predecrement stores descending: d0 ends up at the lowest address.
    CHECK_EQ(m.memory.read32(kStackTop - 16), 0x11111111u);
    CHECK_EQ(m.memory.read32(kStackTop - 4), 0x44444444u);

    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.a[7], kStackTop);
    CHECK_EQ(m.cpu.d[3], 0x11111111u);
    CHECK_EQ(m.cpu.d[4], 0x22222222u);
    CHECK_EQ(m.cpu.d[5], 0x33333333u);
    CHECK_EQ(m.cpu.a[1], 0x44444444u);
}

TEST_CASE("movem.w to registers sign-extends") {
    Machine m;
    m.load_words({0x4c98, 0x0001});   // movem.w (a0)+,d0
    m.memory.write16(0x3000, 0xfffe);
    m.cpu.a[0] = 0x3000;
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.d[0], 0xfffffffeu);
    CHECK_EQ(m.cpu.a[0], 0x3002u);
}

TEST_CASE("dbcc decrements, branches, and falls through at -1") {
    Machine m;
    m.load_words({0x51c8, 0xfffe});   // dbra d0,self
    m.cpu.d[0] = 2;
    m.cpu.pc = 0x1000;

    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.d[0] & 0xffff, 1u);
    CHECK_EQ(m.cpu.pc, 0x1000u);      // branched back

    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.d[0] & 0xffff, 0u);
    CHECK_EQ(m.cpu.pc, 0x1000u);

    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.d[0] & 0xffff, 0xffffu);
    CHECK_EQ(m.cpu.pc, 0x1004u);      // fell through
}

TEST_CASE("dbcc with a true condition falls through without decrementing") {
    Machine m;
    m.load_words({0x57c8, 0xfffe});   // dbeq d0,self
    m.cpu.d[0] = 5;
    m.cpu.z = 1;
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.d[0] & 0xffff, 5u);
    CHECK_EQ(m.cpu.pc, 0x1004u);
}

TEST_CASE("link and unlk build and tear down a frame") {
    Machine m;
    m.load_words({0x4e56, 0xfff8, 0x4e5e});   // link a6,#-8 ; unlk a6
    m.cpu.a[6] = 0xdeadbeef;

    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.a[6], kStackTop - 4);      // frame pointer
    CHECK_EQ(m.cpu.a[7], kStackTop - 12);     // 4 pushed + 8 reserved
    CHECK_EQ(m.memory.read32(kStackTop - 4), 0xdeadbeefu);

    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.a[6], 0xdeadbeefu);
    CHECK_EQ(m.cpu.a[7], kStackTop);
}

TEST_CASE("an illegal instruction takes the illegal vector, it is not skipped") {
    Machine m;
    m.memory.write32(vector_address(Vector::IllegalInstruction), 0x00005000);
    m.load_words({0x4afc});
    m.runtime.step_block(m.cpu);

    CHECK_EQ(m.cpu.pc, 0x00005000u);
    // The faulting instruction's own address is stacked, not the next one.
    CHECK_EQ(m.memory.read32(m.cpu.a[7] + 2), 0x1000u);
    CHECK_EQ(m.runtime.stats().exceptions_taken, 1u);
}

TEST_CASE("line a and line f reach their own vectors") {
    Machine m;
    m.memory.write32(vector_address(Vector::LineA), 0x00006000);
    m.memory.write32(vector_address(Vector::LineF), 0x00007000);

    m.load_words({0xa123});
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.pc, 0x00006000u);

    m.cpu.pc = 0x1000;
    m.load_words({0xf123});
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.pc, 0x00007000u);
}

TEST_CASE("trap stacks the following instruction, not itself") {
    Machine m;
    m.memory.write32(vector_address(static_cast<u32>(Vector::TrapBase) + 3),
                     0x00008000);
    m.load_words({0x4e43});   // trap #3
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.pc, 0x00008000u);
    CHECK_EQ(m.memory.read32(m.cpu.a[7] + 2), 0x1002u);
}

TEST_CASE("a privileged instruction in user mode raises a privilege violation") {
    Machine m;
    m.memory.write32(vector_address(Vector::PrivilegeViolation), 0x00009000);
    m.cpu.set_sr(0x0000);     // user mode
    m.cpu.a[7] = 0x00060000;  // user stack
    m.cpu.ssp = kStackTop;
    m.load_words({0x46fc, 0x2700});   // move.w #$2700,sr

    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.pc, 0x00009000u);
    CHECK(m.cpu.s);                               // now in supervisor mode
    CHECK_EQ(m.cpu.usp, 0x00060000u);             // user stack was preserved
    CHECK_EQ(m.cpu.a[7], kStackTop - 6);          // frame is on the supervisor stack
}

TEST_CASE("divide by zero takes its vector without writing a result") {
    Machine m;
    m.memory.write32(vector_address(Vector::DivideByZero), 0x0000a000);
    m.load_words({0x80fc, 0x0000});   // divu #0,d0
    m.cpu.d[0] = 0x1234;
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.pc, 0x0000a000u);
    CHECK_EQ(m.cpu.d[0], 0x1234u);
}

TEST_CASE("chk raises only when the value is out of bounds") {
    Machine m;
    m.memory.write32(vector_address(Vector::ChkInstruction), 0x0000b000);
    m.load_words({0x41bc, 0x000a});   // chk.w #10,d0

    m.cpu.d[0] = 5;
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.pc, 0x1004u);      // in range: no exception

    m.cpu.pc = 0x1000;
    m.cpu.d[0] = 20;
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.pc, 0x0000b000u);
}

TEST_CASE("an address error carries the faulting address into the frame") {
    Machine m;
    m.memory.write32(vector_address(Vector::AddressError), 0x0000c000);
    m.load_words({0x3010});           // move.w (a0),d0
    m.cpu.a[0] = 0x2001;              // odd
    // Give the handler something to do, so the test observes the frame rather
    // than executing whatever happens to follow it.
    m.load_words({0x33fc, 0x0001, 0x00f0, 0x0000}, 0x0000c000);

    m.runtime.run(m.cpu, 10000);
    CHECK(m.cpu.halted);
    CHECK_EQ(m.runtime.stats().exceptions_taken, 1u);

    // The 14-byte group 0 frame: status word, access address, instruction
    // register, status register, program counter.
    const u32 frame = m.cpu.a[7];
    CHECK_EQ(m.memory.read32(frame + 2), 0x2001u);   // faulting address
    CHECK_EQ(m.memory.read16(frame + 6), 0x3010u);   // the offending opcode
    CHECK_EQ(m.memory.read16(frame) & 0x10u, 0x10u); // it was a read
}

TEST_CASE("rte restores the status register and the return address") {
    Machine m;
    m.load_words({0x4e73});           // rte
    m.cpu.a[7] = kStackTop - 6;
    m.memory.write16(kStackTop - 6, 0x0000);      // user mode, no flags
    m.memory.write32(kStackTop - 4, 0x00004000);
    m.cpu.usp = 0x00060000;

    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.cpu.pc, 0x00004000u);
    CHECK(!m.cpu.s);
    CHECK_EQ(m.cpu.a[7], 0x00060000u);   // swapped back to the user stack
}

TEST_CASE("movep moves alternate bytes") {
    Machine m;
    m.load_words({0x01c8, 0x0000});   // movep.l d0,0(a0)
    m.cpu.d[0] = 0xaabbccdd;
    m.cpu.a[0] = 0x3000;
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.memory.read8(0x3000), 0xaau);
    CHECK_EQ(m.memory.read8(0x3002), 0xbbu);
    CHECK_EQ(m.memory.read8(0x3004), 0xccu);
    CHECK_EQ(m.memory.read8(0x3006), 0xddu);
    CHECK_EQ(m.memory.read8(0x3001), 0x00u);   // untouched
}

TEST_CASE("tas sets the high bit after reading") {
    Machine m;
    m.load_words({0x4ad0});   // tas (a0)
    m.cpu.a[0] = 0x3000;
    m.memory.write8(0x3000, 0x01);
    m.runtime.step_block(m.cpu);
    CHECK_EQ(m.memory.read8(0x3000), 0x81u);
    CHECK_EQ(m.cpu.z, 0u);
    CHECK_EQ(m.cpu.n, 0u);
}

TEST_CASE("stop halts until an interrupt and keeps guest time moving") {
    Machine m;
    m.load_words({0x4e72, 0x2000});   // stop #$2000
    m.runtime.step_block(m.cpu);
    CHECK(m.cpu.stopped);
    CHECK_EQ(m.cpu.int_mask, 0u);
    // With no hardware attached there is nothing to wake it, and run() must
    // return rather than spin forever.
    m.runtime.run(m.cpu, 1000);
    CHECK(m.cpu.stopped);
}

ARECOMP_TEST_MAIN()
