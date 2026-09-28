#include "amiga_recomp/decoder.hpp"

#include <vector>

#include "test_support.hpp"

using namespace arecomp;

namespace {

// Assemble a sequence of big-endian words at guest address 0x1000 and decode
// the instruction at the start.
DecodedInstruction decode_words(std::initializer_list<u16> words, u32 base = 0x1000) {
    std::vector<u8> bytes;
    for (u16 w : words) {
        bytes.push_back(static_cast<u8>(w >> 8));
        bytes.push_back(static_cast<u8>(w));
    }
    bytes.resize(bytes.size() + 8, 0);   // decode never reads past the end
    return decode_buffer(bytes.data(), bytes.size(), base, base);
}

} // namespace

TEST_CASE("move.w d0,d1") {
    const auto in = decode_words({0x3200});
    CHECK(in.valid());
    CHECK(in.mnemonic == Mnemonic::Move);
    CHECK(in.size == Size::Word);
    CHECK(in.src.kind == EAKind::DataReg);
    CHECK_EQ(in.src.reg, 0u);
    CHECK(in.dst.kind == EAKind::DataReg);
    CHECK_EQ(in.dst.reg, 1u);
    CHECK_EQ(in.length, 2u);
}

TEST_CASE("move.l (a0)+,-(a1) keeps both modes and sizes") {
    const auto in = decode_words({0x2318});
    CHECK(in.mnemonic == Mnemonic::Move);
    CHECK(in.size == Size::Long);
    CHECK(in.src.kind == EAKind::PostInc);
    CHECK_EQ(in.src.reg, 0u);
    CHECK(in.dst.kind == EAKind::PreDec);
    CHECK_EQ(in.dst.reg, 1u);
}

TEST_CASE("movea.w decodes as movea, not move") {
    const auto in = decode_words({0x3040});
    CHECK(in.mnemonic == Mnemonic::Movea);
    CHECK(in.size == Size::Word);
    CHECK(in.dst.kind == EAKind::AddrReg);
    CHECK_EQ(in.dst.reg, 0u);
}

TEST_CASE("move.b with an address register operand is illegal") {
    const auto in = decode_words({0x1008});   // move.b a0,d0
    CHECK(!in.valid());
}

TEST_CASE("moveq sign-extends its byte") {
    const auto in = decode_words({0x70ff});
    CHECK(in.mnemonic == Mnemonic::Moveq);
    CHECK(in.size == Size::Long);
    CHECK_EQ(in.src.value, 0xffffffffu);
    CHECK_EQ(in.dst.reg, 0u);
}

TEST_CASE("d16(an) sign-extends the displacement") {
    const auto in = decode_words({0x3028, 0xfffc});   // move.w -4(a0),d0
    CHECK(in.src.kind == EAKind::Displacement);
    CHECK_EQ(in.src.displacement, -4);
    CHECK_EQ(in.length, 4u);
}

TEST_CASE("absolute word sign-extends to a full guest address") {
    const auto in = decode_words({0x3038, 0x8000});   // move.w $ffff8000,d0
    CHECK(in.src.kind == EAKind::AbsW);
    CHECK_EQ(in.src.value, 0xffff8000u);
    CHECK_EQ(in.length, 4u);
}

TEST_CASE("absolute long keeps all four bytes") {
    const auto in = decode_words({0x3039, 0x0012, 0x3456});
    CHECK(in.src.kind == EAKind::AbsL);
    CHECK_EQ(in.src.value, 0x00123456u);
    CHECK_EQ(in.length, 6u);
}

TEST_CASE("d16(pc) resolves against the extension word address") {
    // move.w 4(pc),d0 at 0x1000: the extension word is at 0x1002.
    const auto in = decode_words({0x303a, 0x0004});
    CHECK(in.src.kind == EAKind::PcDisplacement);
    CHECK_EQ(in.src.value, 0x1006u);
    CHECK(in.src.is_static_address());
}

TEST_CASE("d8(an,xn) decodes the index register specification") {
    // move.w 8(a0,d1.l),d0
    const auto in = decode_words({0x3030, 0x1808});
    CHECK(in.src.kind == EAKind::IndexIndirect);
    CHECK_EQ(in.src.displacement, 8);
    CHECK_EQ(in.src.index.reg, 1u);
    CHECK(!in.src.index.address);
    CHECK(in.src.index.long_size);
}

TEST_CASE("immediate size follows the operation size") {
    const auto b = decode_words({0x0600, 0x0012});          // addi.b #$12,d0
    CHECK(b.mnemonic == Mnemonic::Addi);
    CHECK(b.size == Size::Byte);
    CHECK_EQ(b.src.value, 0x12u);
    CHECK_EQ(b.length, 4u);

    const auto l = decode_words({0x0680, 0x1234, 0x5678});  // addi.l #...,d0
    CHECK(l.size == Size::Long);
    CHECK_EQ(l.src.value, 0x12345678u);
    CHECK_EQ(l.length, 6u);
}

TEST_CASE("branch targets are relative to pc+2") {
    const auto in = decode_words({0x6002});   // bra.s *+4
    CHECK(in.mnemonic == Mnemonic::Bra);
    CHECK_EQ(in.target, 0x1004u);
    CHECK(in.flags & INSN_ENDS_BLOCK);
    CHECK(!(in.flags & INSN_CONDITIONAL));
}

TEST_CASE("a zero displacement selects the word branch form") {
    const auto in = decode_words({0x6000, 0x0100});
    CHECK(in.mnemonic == Mnemonic::Bra);
    CHECK_EQ(in.target, 0x1102u);
    CHECK_EQ(in.length, 4u);
}

TEST_CASE("0xff is a displacement of -1, not a long branch on the 68000") {
    const auto in = decode_words({0x60ff});
    CHECK(in.mnemonic == Mnemonic::Bra);
    CHECK_EQ(in.target, 0x1001u);
    CHECK_EQ(in.length, 2u);
}

TEST_CASE("bsr is a call and bne is conditional") {
    const auto bsr = decode_words({0x6100});
    CHECK(bsr.mnemonic == Mnemonic::Bsr);
    CHECK(bsr.flags & INSN_CALL);

    const auto bne = decode_words({0x6604});
    CHECK(bne.mnemonic == Mnemonic::Bcc);
    CHECK(bne.condition == Cond::NE);
    CHECK(bne.flags & INSN_CONDITIONAL);
}

TEST_CASE("dbcc decodes its register, condition and target") {
    const auto in = decode_words({0x51c8, 0xfffc});   // dbra d0,*-2
    CHECK(in.mnemonic == Mnemonic::Dbcc);
    CHECK(in.condition == Cond::F);
    CHECK_EQ(in.dst.reg, 0u);
    CHECK_EQ(in.target, 0x0ffeu);
    CHECK(in.flags & INSN_CONDITIONAL);
}

TEST_CASE("scc is not mistaken for dbcc") {
    const auto in = decode_words({0x57c0});   // seq d0
    CHECK(in.mnemonic == Mnemonic::Scc);
    CHECK(in.condition == Cond::EQ);
    CHECK(in.dst.kind == EAKind::DataReg);
}

TEST_CASE("addq of zero means eight") {
    const auto in = decode_words({0x5040});   // addq.w #8,d0
    CHECK(in.mnemonic == Mnemonic::Addq);
    CHECK_EQ(in.src.value, 8u);
}

TEST_CASE("jmp and jsr mark indirect targets") {
    const auto direct = decode_words({0x4ef9, 0x0001, 0x2000});
    CHECK(direct.mnemonic == Mnemonic::Jmp);
    CHECK_EQ(direct.target, 0x00012000u);
    CHECK(!(direct.flags & INSN_INDIRECT));

    const auto indirect = decode_words({0x4ed0});   // jmp (a0)
    CHECK(indirect.mnemonic == Mnemonic::Jmp);
    CHECK(indirect.flags & INSN_INDIRECT);
    CHECK(indirect.flags & INSN_ENDS_BLOCK);

    const auto call = decode_words({0x4e90});   // jsr (a0)
    CHECK(call.mnemonic == Mnemonic::Jsr);
    CHECK(call.flags & INSN_CALL);
    CHECK(call.flags & INSN_INDIRECT);
}

TEST_CASE("rts and rte end a block and return") {
    const auto rts = decode_words({0x4e75});
    CHECK(rts.mnemonic == Mnemonic::Rts);
    CHECK(rts.flags & INSN_RETURN);
    CHECK(rts.flags & INSN_ENDS_BLOCK);

    const auto rte = decode_words({0x4e73});
    CHECK(rte.mnemonic == Mnemonic::Rte);
    CHECK(rte.flags & INSN_PRIVILEGED);
}

TEST_CASE("lea and pea keep the addressing mode, not a loaded value") {
    const auto lea = decode_words({0x41f9, 0x0002, 0x0000});
    CHECK(lea.mnemonic == Mnemonic::Lea);
    CHECK(lea.src.kind == EAKind::AbsL);
    CHECK_EQ(lea.src.value, 0x00020000u);
    CHECK(lea.dst.kind == EAKind::AddrReg);

    const auto pea = decode_words({0x4850});   // pea (a0)
    CHECK(pea.mnemonic == Mnemonic::Pea);
    CHECK(pea.src.kind == EAKind::Indirect);
}

TEST_CASE("swap is distinguished from pea") {
    const auto in = decode_words({0x4840});
    CHECK(in.mnemonic == Mnemonic::Swap);
    CHECK_EQ(in.dst.reg, 0u);
}

TEST_CASE("ext is distinguished from movem") {
    const auto ext_w = decode_words({0x4880});
    CHECK(ext_w.mnemonic == Mnemonic::Ext);
    CHECK(ext_w.size == Size::Word);

    const auto ext_l = decode_words({0x48c0});
    CHECK(ext_l.mnemonic == Mnemonic::Ext);
    CHECK(ext_l.size == Size::Long);

    // movem.l d0-d7/a0-a6,-(a7)
    const auto movem = decode_words({0x48e7, 0xfffe});
    CHECK(movem.mnemonic == Mnemonic::Movem);
    CHECK(movem.size == Size::Long);
    CHECK(movem.src.kind == EAKind::RegList);
    CHECK_EQ(movem.src.value, 0xfffeu);
    CHECK(movem.dst.kind == EAKind::PreDec);

    // movem.l (a7)+,d0-d7/a0-a6
    const auto restore = decode_words({0x4cdf, 0x7fff});
    CHECK(restore.mnemonic == Mnemonic::Movem);
    CHECK(restore.src.kind == EAKind::PostInc);
    CHECK(restore.dst.kind == EAKind::RegList);
}

TEST_CASE("movep is not decoded as a dynamic bit operation") {
    const auto in = decode_words({0x0188, 0x0004});   // movep.w d0,4(a0)
    CHECK(in.mnemonic == Mnemonic::Movep);
    CHECK(in.size == Size::Word);
    CHECK(in.src.kind == EAKind::DataReg);
    CHECK(in.dst.kind == EAKind::Displacement);
    CHECK_EQ(in.dst.displacement, 4);
}

TEST_CASE("static and dynamic bit operations use different operand sources") {
    const auto stat = decode_words({0x0800, 0x0003});   // btst #3,d0
    CHECK(stat.mnemonic == Mnemonic::Btst);
    CHECK(stat.src.kind == EAKind::Immediate);
    CHECK_EQ(stat.src.value, 3u);
    CHECK(stat.size == Size::Long);   // register destination: long

    const auto dyn = decode_words({0x0110});   // btst d0,(a0)
    CHECK(dyn.mnemonic == Mnemonic::Btst);
    CHECK(dyn.src.kind == EAKind::DataReg);
    CHECK(dyn.size == Size::Byte);    // memory destination: byte
}

TEST_CASE("immediate to ccr and sr are not ordinary ori") {
    const auto ccr = decode_words({0x003c, 0x0010});
    CHECK(ccr.mnemonic == Mnemonic::OriToCcr);
    CHECK(ccr.dst.kind == EAKind::Ccr);

    const auto sr = decode_words({0x007c, 0x0700});
    CHECK(sr.mnemonic == Mnemonic::OriToSr);
    CHECK(sr.flags & INSN_PRIVILEGED);
}

TEST_CASE("arithmetic direction is decoded from the opmode field") {
    const auto to_reg = decode_words({0xd041});     // add.w d1,d0
    CHECK(to_reg.mnemonic == Mnemonic::Add);
    CHECK(to_reg.src.kind == EAKind::DataReg);
    CHECK_EQ(to_reg.src.reg, 1u);
    CHECK(to_reg.dst.kind == EAKind::DataReg);
    CHECK_EQ(to_reg.dst.reg, 0u);

    const auto to_mem = decode_words({0xd150});     // add.w d0,(a0)
    CHECK(to_mem.mnemonic == Mnemonic::Add);
    CHECK(to_mem.src.kind == EAKind::DataReg);
    CHECK(to_mem.dst.kind == EAKind::Indirect);
}

TEST_CASE("adda and suba select their size from the opmode") {
    const auto w = decode_words({0xd0c1});   // adda.w d1,a0
    CHECK(w.mnemonic == Mnemonic::Adda);
    CHECK(w.size == Size::Word);

    const auto l = decode_words({0x91c9});   // suba.l a1,a0
    CHECK(l.mnemonic == Mnemonic::Suba);
    CHECK(l.size == Size::Long);
}

TEST_CASE("addx and subx separate register and memory forms") {
    const auto reg = decode_words({0xd301});   // addx.b d1,d1
    CHECK(reg.mnemonic == Mnemonic::Addx);
    CHECK(reg.src.kind == EAKind::DataReg);

    const auto mem = decode_words({0xd309});   // addx.b -(a1),-(a1)
    CHECK(mem.mnemonic == Mnemonic::Addx);
    CHECK(mem.src.kind == EAKind::PreDec);
    CHECK(mem.dst.kind == EAKind::PreDec);
}

TEST_CASE("cmpm is not decoded as eor") {
    const auto in = decode_words({0xb308});   // cmpm.b (a0)+,(a1)+
    CHECK(in.mnemonic == Mnemonic::Cmpm);
    CHECK(in.src.kind == EAKind::PostInc);
    CHECK(in.dst.kind == EAKind::PostInc);

    const auto eor = decode_words({0xb300});  // eor.b d1,d0
    CHECK(eor.mnemonic == Mnemonic::Eor);
}

TEST_CASE("abcd and sbcd separate register and memory forms") {
    const auto reg = decode_words({0xc300});   // abcd d0,d1
    CHECK(reg.mnemonic == Mnemonic::Abcd);
    CHECK(reg.src.kind == EAKind::DataReg);

    const auto mem = decode_words({0x8309});   // sbcd -(a1),-(a1)
    CHECK(mem.mnemonic == Mnemonic::Sbcd);
    CHECK(mem.src.kind == EAKind::PreDec);
}

TEST_CASE("exg decodes all three register combinations") {
    const auto dd = decode_words({0xc141});
    CHECK(dd.mnemonic == Mnemonic::Exg);
    CHECK(dd.src.kind == EAKind::DataReg);
    CHECK(dd.dst.kind == EAKind::DataReg);

    const auto aa = decode_words({0xc149});
    CHECK(aa.mnemonic == Mnemonic::Exg);
    CHECK(aa.src.kind == EAKind::AddrReg);
    CHECK(aa.dst.kind == EAKind::AddrReg);

    const auto da = decode_words({0xc189});
    CHECK(da.mnemonic == Mnemonic::Exg);
    CHECK(da.src.kind == EAKind::DataReg);
    CHECK(da.dst.kind == EAKind::AddrReg);
}

TEST_CASE("multiply and divide take a word source") {
    const auto mulu = decode_words({0xc0c1});
    CHECK(mulu.mnemonic == Mnemonic::Mulu);
    CHECK(mulu.size == Size::Word);

    const auto muls = decode_words({0xc1c1});
    CHECK(muls.mnemonic == Mnemonic::Muls);

    const auto divu = decode_words({0x80c1});
    CHECK(divu.mnemonic == Mnemonic::Divu);
    CHECK(divu.flags & INSN_TRAP);

    const auto divs = decode_words({0x81c1});
    CHECK(divs.mnemonic == Mnemonic::Divs);
}

TEST_CASE("shifts decode type, direction, size and count source") {
    const auto asl_imm = decode_words({0xe340});   // asl.w #1,d0
    CHECK(asl_imm.mnemonic == Mnemonic::Asl);
    CHECK(asl_imm.size == Size::Word);
    CHECK(asl_imm.src.kind == EAKind::Immediate);
    CHECK_EQ(asl_imm.src.value, 1u);

    const auto lsr_reg = decode_words({0xe2a8});   // lsr.l d1,d0
    CHECK(lsr_reg.mnemonic == Mnemonic::Lsr);
    CHECK(lsr_reg.size == Size::Long);
    CHECK(lsr_reg.src.kind == EAKind::DataReg);
    CHECK_EQ(lsr_reg.src.reg, 1u);

    const auto roxl = decode_words({0xe550});      // roxl.w #2,d0
    CHECK(roxl.mnemonic == Mnemonic::Roxl);
    CHECK_EQ(roxl.src.value, 2u);

    const auto ror_mem = decode_words({0xe6d0});   // ror.w (a0)
    CHECK(ror_mem.mnemonic == Mnemonic::Ror);
    CHECK(ror_mem.size == Size::Word);
    CHECK(ror_mem.dst.kind == EAKind::Indirect);
    CHECK_EQ(ror_mem.src.value, 1u);
}

TEST_CASE("a shift count of zero in the immediate form means eight") {
    const auto in = decode_words({0xe100});   // asl.b #8,d0
    CHECK(in.mnemonic == Mnemonic::Asl);
    CHECK_EQ(in.src.value, 8u);
}

TEST_CASE("trap, link and unlk decode their operands") {
    const auto trap = decode_words({0x4e4f});
    CHECK(trap.mnemonic == Mnemonic::Trap);
    CHECK_EQ(trap.src.value, 15u);

    const auto link = decode_words({0x4e56, 0xfff8});
    CHECK(link.mnemonic == Mnemonic::Link);
    CHECK_EQ(link.dst.reg, 6u);
    CHECK_EQ(link.src.value, 0xfff8u);

    const auto unlk = decode_words({0x4e5e});
    CHECK(unlk.mnemonic == Mnemonic::Unlk);
    CHECK_EQ(unlk.dst.reg, 6u);
}

TEST_CASE("stop consumes its immediate word") {
    const auto in = decode_words({0x4e72, 0x2000});
    CHECK(in.mnemonic == Mnemonic::Stop);
    CHECK_EQ(in.src.value, 0x2000u);
    CHECK_EQ(in.length, 4u);
    CHECK(in.flags & INSN_PRIVILEGED);
}

TEST_CASE("line a and line f are reported, never silently skipped") {
    const auto a = decode_words({0xa000});
    CHECK(a.mnemonic == Mnemonic::LineA);
    CHECK(!a.valid());

    const auto f = decode_words({0xf000});
    CHECK(f.mnemonic == Mnemonic::LineF);
    CHECK(!f.valid());

    const auto illegal = decode_words({0x4afc});
    CHECK(illegal.mnemonic == Mnemonic::IllegalOp);
}

TEST_CASE("disassembly is readable enough for generated-code comments") {
    CHECK_STR_EQ(disassemble(decode_words({0x3200})), "move.w d0,d1");
    CHECK_STR_EQ(disassemble(decode_words({0x4e75})), "rts");
    CHECK_STR_EQ(disassemble(decode_words({0x6002})), "bra.b $00001004");
}

ARECOMP_TEST_MAIN()
