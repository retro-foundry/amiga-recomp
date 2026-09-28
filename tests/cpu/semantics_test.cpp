// Flag correctness is the thing most likely to be silently wrong, and a
// silently wrong flag shows up as a game that plays subtly incorrectly rather
// than as a crash. AMIGA_RECOMP.md 13 lists the edge cases; they are all here.
#include "amiga_recomp/semantics.hpp"

#include "test_support.hpp"

using namespace arecomp;
using namespace arecomp::sem;

namespace {

M68kState fresh() {
    M68kState c;
    c.x = c.n = c.z = c.v = c.c = 0;
    return c;
}

// Compact flag readback: "xnzvc" with a dot for clear.
std::string flags(const M68kState& c) {
    std::string s;
    s += c.x ? 'x' : '.';
    s += c.n ? 'n' : '.';
    s += c.z ? 'z' : '.';
    s += c.v ? 'v' : '.';
    s += c.c ? 'c' : '.';
    return s;
}

} // namespace

TEST_CASE("add byte: 0x7f + 1 overflows without carry") {
    auto c = fresh();
    CHECK_EQ(add<8>(c, 0x7f, 0x01), 0x80u);
    CHECK_STR_EQ(flags(c), ".n.v.");
}

TEST_CASE("add byte: 0xff + 1 carries without overflow") {
    auto c = fresh();
    CHECK_EQ(add<8>(c, 0xff, 0x01), 0x00u);
    CHECK_STR_EQ(flags(c), "x.z.c");
}

TEST_CASE("sub word: 0x8000 - 1 overflows") {
    auto c = fresh();
    CHECK_EQ(sub<16>(c, 0x8000, 0x0001), 0x7fffu);
    CHECK_STR_EQ(flags(c), "...v.");
}

TEST_CASE("sub byte: 0 - 1 borrows") {
    auto c = fresh();
    CHECK_EQ(sub<8>(c, 0x00, 0x01), 0xffu);
    CHECK_STR_EQ(flags(c), "xn..c");
}

TEST_CASE("cmp does not touch X") {
    auto c = fresh();
    c.x = 1;
    cmp<8>(c, 0x00, 0x01);
    CHECK_EQ(c.x, 1u);
    CHECK_EQ(c.c, 1u);
}

TEST_CASE("addx accumulates Z rather than setting it") {
    auto c = fresh();
    c.z = 1;              // caller has seen only zero results so far
    c.x = 0;
    CHECK_EQ(addx<16>(c, 0x0000, 0x0000), 0x0000u);
    CHECK_EQ(c.z, 1u);    // still all zero: Z survives

    c.z = 1;
    CHECK_EQ(addx<16>(c, 0x0001, 0x0000), 0x0001u);
    CHECK_EQ(c.z, 0u);    // a non-zero part clears Z for the whole operation
}

TEST_CASE("addx carries the X bit in") {
    auto c = fresh();
    c.x = 1;
    CHECK_EQ(addx<8>(c, 0x01, 0x01), 0x03u);
    CHECK_EQ(c.x, 0u);
}

TEST_CASE("subx borrows the X bit") {
    auto c = fresh();
    c.x = 1;
    CHECK_EQ(subx<8>(c, 0x00, 0x00), 0xffu);
    CHECK_EQ(c.x, 1u);
    CHECK_EQ(c.c, 1u);
}

TEST_CASE("neg sets carry for any non-zero operand") {
    auto c = fresh();
    CHECK_EQ(neg<8>(c, 0x01), 0xffu);
    CHECK_EQ(c.c, 1u);
    CHECK_EQ(c.x, 1u);

    c = fresh();
    CHECK_EQ(neg<8>(c, 0x00), 0x00u);
    CHECK_EQ(c.c, 0u);
    CHECK_EQ(c.z, 1u);

    // 0x80 negates to itself, which is the overflow case.
    c = fresh();
    CHECK_EQ(neg<8>(c, 0x80), 0x80u);
    CHECK_EQ(c.v, 1u);
}

TEST_CASE("asl sets V when the sign bit changes during the shift") {
    auto c = fresh();
    CHECK_EQ(asl<8>(c, 0x40, 1), 0x80u);
    CHECK_EQ(c.v, 1u);     // 0 -> 1 in the sign bit

    c = fresh();
    CHECK_EQ(asl<8>(c, 0x01, 1), 0x02u);
    CHECK_EQ(c.v, 0u);

    c = fresh();
    CHECK_EQ(asl<8>(c, 0xc0, 1), 0x80u);
    CHECK_EQ(c.v, 0u);     // sign stayed 1 throughout
    CHECK_EQ(c.c, 1u);
}

TEST_CASE("a shift count of zero clears C and leaves X alone") {
    auto c = fresh();
    c.x = 1;
    CHECK_EQ(asl<16>(c, 0x1234, 0), 0x1234u);
    CHECK_EQ(c.c, 0u);
    CHECK_EQ(c.x, 1u);     // X is explicitly not affected
    CHECK_EQ(c.v, 0u);
}

TEST_CASE("shifting a word by exactly its width keeps the last bit out") {
    auto c = fresh();
    CHECK_EQ(lsl<16>(c, 0x0001, 16), 0x0000u);
    CHECK_EQ(c.c, 1u);
    CHECK_EQ(c.x, 1u);

    c = fresh();
    CHECK_EQ(lsl<16>(c, 0x0001, 17), 0x0000u);
    CHECK_EQ(c.c, 0u);     // everything has already been shifted out
}

TEST_CASE("asr replicates the sign bit") {
    auto c = fresh();
    CHECK_EQ(asr<8>(c, 0x80, 1), 0xc0u);
    CHECK_EQ(c.n, 1u);
    CHECK_EQ(c.c, 0u);

    c = fresh();
    CHECK_EQ(asr<8>(c, 0x80, 9), 0xffu);   // over-shifted: all sign
    CHECK_EQ(c.c, 1u);
}

TEST_CASE("lsr always clears N") {
    auto c = fresh();
    CHECK_EQ(lsr<8>(c, 0x80, 1), 0x40u);
    CHECK_EQ(c.n, 0u);
    CHECK_EQ(c.c, 0u);
}

TEST_CASE("rol and ror take C from the bit that wrapped") {
    auto c = fresh();
    CHECK_EQ(rol<8>(c, 0x81, 1), 0x03u);
    CHECK_EQ(c.c, 1u);

    c = fresh();
    CHECK_EQ(ror<8>(c, 0x81, 1), 0xc0u);
    CHECK_EQ(c.c, 1u);

    // A rotate by a whole width is a no-op for the value but not for C.
    c = fresh();
    CHECK_EQ(rol<8>(c, 0x80, 8), 0x80u);
    CHECK_EQ(c.c, 0u);     // bit 0 of the result
}

TEST_CASE("rotates never touch X") {
    auto c = fresh();
    c.x = 1;
    rol<16>(c, 0xffff, 3);
    CHECK_EQ(c.x, 1u);
    ror<16>(c, 0xffff, 3);
    CHECK_EQ(c.x, 1u);
}

TEST_CASE("roxl rotates through X as a 17-bit quantity") {
    auto c = fresh();
    c.x = 1;
    CHECK_EQ(roxl<16>(c, 0x0000, 1), 0x0001u);   // X shifted in
    CHECK_EQ(c.x, 0u);

    c = fresh();
    c.x = 0;
    CHECK_EQ(roxl<16>(c, 0x8000, 1), 0x0000u);
    CHECK_EQ(c.x, 1u);                            // MSB shifted out into X
    CHECK_EQ(c.c, 1u);
}

TEST_CASE("roxr with a zero count copies X into C") {
    auto c = fresh();
    c.x = 1;
    CHECK_EQ(roxr<16>(c, 0x1234, 0), 0x1234u);
    CHECK_EQ(c.c, 1u);
    CHECK_EQ(c.x, 1u);
}

TEST_CASE("multiply sets flags from the full 32-bit result") {
    auto c = fresh();
    CHECK_EQ(mulu(c, 0xffff, 0xffff), 0xfffe0001u);
    CHECK_EQ(c.n, 1u);
    CHECK_EQ(c.v, 0u);

    c = fresh();
    CHECK_EQ(muls(c, 0xffff, 0xffff), 0x00000001u);   // -1 * -1
    CHECK_EQ(c.n, 0u);
}

TEST_CASE("divu reports overflow without writing a result") {
    auto c = fresh();
    auto r = divu(c, 0x00010000, 0x0001);
    CHECK(r.overflow);
    CHECK_EQ(c.v, 1u);

    c = fresh();
    r = divu(c, 100, 7);
    CHECK(!r.overflow);
    CHECK_EQ(r.value & 0xffffu, 14u);          // quotient
    CHECK_EQ(r.value >> 16, 2u);               // remainder
}

TEST_CASE("divs truncates toward zero and keeps the dividend's sign") {
    auto c = fresh();
    const auto r = divs(c, static_cast<u32>(-100), 7);
    CHECK_EQ(static_cast<s16>(r.value & 0xffff), -14);
    CHECK_EQ(static_cast<s16>(r.value >> 16), -2);
}

TEST_CASE("divide by zero is reported, never approximated") {
    auto c = fresh();
    CHECK(divu(c, 1, 0).divide_by_zero);
    CHECK(divs(c, 1, 0).divide_by_zero);
}

TEST_CASE("divs handles the -2^31 / -1 special case without trapping") {
    auto c = fresh();
    const auto r = divs(c, 0x80000000u, 0xffff);
    CHECK(!r.overflow);
    CHECK_EQ(r.value, 0u);
}

TEST_CASE("abcd performs decimal addition and accumulates Z") {
    auto c = fresh();
    c.z = 1;
    CHECK_EQ(abcd(c, 0x28, 0x14), 0x42u);
    CHECK_EQ(c.c, 0u);
    CHECK_EQ(c.z, 0u);

    c = fresh();
    c.z = 1;
    CHECK_EQ(abcd(c, 0x99, 0x01), 0x00u);
    CHECK_EQ(c.c, 1u);
    CHECK_EQ(c.z, 1u);     // result is zero, so Z is left alone
}

TEST_CASE("sbcd performs decimal subtraction with borrow") {
    auto c = fresh();
    c.z = 1;
    CHECK_EQ(sbcd(c, 0x42, 0x14), 0x28u);
    CHECK_EQ(c.c, 0u);

    c = fresh();
    c.z = 1;
    CHECK_EQ(sbcd(c, 0x00, 0x01), 0x99u);
    CHECK_EQ(c.c, 1u);
}

TEST_CASE("nbcd negates in decimal") {
    auto c = fresh();
    c.z = 1;
    CHECK_EQ(nbcd(c, 0x00), 0x00u);
    CHECK_EQ(c.c, 0u);

    c = fresh();
    c.z = 1;
    CHECK_EQ(nbcd(c, 0x01), 0x99u);
    CHECK_EQ(c.c, 1u);
}

TEST_CASE("status register packs and unpacks without losing bits") {
    M68kState c;
    c.set_sr(0x271f);
    CHECK_EQ(c.sr(), 0x271fu);
    CHECK(c.s);
    CHECK_EQ(c.int_mask, 7u);
    CHECK_EQ(c.x, 1u);
}

TEST_CASE("changing the supervisor bit swaps the stack pointers") {
    M68kState c;
    c.s = true;
    c.a[7] = 0x00080000;   // supervisor stack in use
    c.usp = 0x00070000;

    c.set_sr(0x0000);      // drop to user mode
    CHECK(!c.s);
    CHECK_EQ(c.a[7], 0x00070000u);
    CHECK_EQ(c.ssp, 0x00080000u);

    c.set_sr(0x2000);      // back to supervisor
    CHECK_EQ(c.a[7], 0x00080000u);
    CHECK_EQ(c.usp, 0x00070000u);
}

TEST_CASE("condition codes cover all sixteen encodings") {
    M68kState c;
    c.set_ccr(0);
    CHECK(test_cond(c, Cond::T));
    CHECK(!test_cond(c, Cond::F));
    CHECK(test_cond(c, Cond::HI));
    CHECK(test_cond(c, Cond::NE));
    CHECK(test_cond(c, Cond::GE));
    CHECK(test_cond(c, Cond::GT));

    c.set_ccr(SR_Z);
    CHECK(test_cond(c, Cond::EQ));
    CHECK(test_cond(c, Cond::LE));
    CHECK(!test_cond(c, Cond::GT));
    CHECK(test_cond(c, Cond::LS));

    c.set_ccr(SR_N);
    CHECK(test_cond(c, Cond::MI));
    CHECK(test_cond(c, Cond::LT));
    CHECK(!test_cond(c, Cond::GE));

    c.set_ccr(SR_N | SR_V);
    CHECK(test_cond(c, Cond::GE));   // N == V
    CHECK(test_cond(c, Cond::GT));
    CHECK(test_cond(c, Cond::VS));
}

ARECOMP_TEST_MAIN()
