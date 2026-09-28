#include "amiga_recomp/decoder.hpp"

#include <array>
#include <cstdio>

namespace arecomp {

const char* mnemonic_name(Mnemonic m) noexcept {
    switch (m) {
    case Mnemonic::Illegal: return "???";
    case Mnemonic::Abcd: return "abcd";
    case Mnemonic::Add: return "add";
    case Mnemonic::Adda: return "adda";
    case Mnemonic::Addi: return "addi";
    case Mnemonic::Addq: return "addq";
    case Mnemonic::Addx: return "addx";
    case Mnemonic::And: return "and";
    case Mnemonic::Andi: return "andi";
    case Mnemonic::AndiToCcr: return "andi_to_ccr";
    case Mnemonic::AndiToSr: return "andi_to_sr";
    case Mnemonic::Asl: return "asl";
    case Mnemonic::Asr: return "asr";
    case Mnemonic::Bcc: return "bcc";
    case Mnemonic::Bchg: return "bchg";
    case Mnemonic::Bclr: return "bclr";
    case Mnemonic::Bra: return "bra";
    case Mnemonic::Bset: return "bset";
    case Mnemonic::Bsr: return "bsr";
    case Mnemonic::Btst: return "btst";
    case Mnemonic::Chk: return "chk";
    case Mnemonic::Clr: return "clr";
    case Mnemonic::Cmp: return "cmp";
    case Mnemonic::Cmpa: return "cmpa";
    case Mnemonic::Cmpi: return "cmpi";
    case Mnemonic::Cmpm: return "cmpm";
    case Mnemonic::Dbcc: return "dbcc";
    case Mnemonic::Divs: return "divs";
    case Mnemonic::Divu: return "divu";
    case Mnemonic::Eor: return "eor";
    case Mnemonic::Eori: return "eori";
    case Mnemonic::EoriToCcr: return "eori_to_ccr";
    case Mnemonic::EoriToSr: return "eori_to_sr";
    case Mnemonic::Exg: return "exg";
    case Mnemonic::Ext: return "ext";
    case Mnemonic::IllegalOp: return "illegal";
    case Mnemonic::Jmp: return "jmp";
    case Mnemonic::Jsr: return "jsr";
    case Mnemonic::Lea: return "lea";
    case Mnemonic::Link: return "link";
    case Mnemonic::Lsl: return "lsl";
    case Mnemonic::Lsr: return "lsr";
    case Mnemonic::Move: return "move";
    case Mnemonic::Movea: return "movea";
    case Mnemonic::MoveFromSr: return "move_from_sr";
    case Mnemonic::MoveToCcr: return "move_to_ccr";
    case Mnemonic::MoveToSr: return "move_to_sr";
    case Mnemonic::MoveUspToAn: return "move_usp_to_an";
    case Mnemonic::MoveAnToUsp: return "move_an_to_usp";
    case Mnemonic::Movem: return "movem";
    case Mnemonic::Movep: return "movep";
    case Mnemonic::Moveq: return "moveq";
    case Mnemonic::Muls: return "muls";
    case Mnemonic::Mulu: return "mulu";
    case Mnemonic::Nbcd: return "nbcd";
    case Mnemonic::Neg: return "neg";
    case Mnemonic::Negx: return "negx";
    case Mnemonic::Nop: return "nop";
    case Mnemonic::Not: return "not";
    case Mnemonic::Or: return "or";
    case Mnemonic::Ori: return "ori";
    case Mnemonic::OriToCcr: return "ori_to_ccr";
    case Mnemonic::OriToSr: return "ori_to_sr";
    case Mnemonic::Pea: return "pea";
    case Mnemonic::Reset: return "reset";
    case Mnemonic::Rol: return "rol";
    case Mnemonic::Ror: return "ror";
    case Mnemonic::Roxl: return "roxl";
    case Mnemonic::Roxr: return "roxr";
    case Mnemonic::Rte: return "rte";
    case Mnemonic::Rtr: return "rtr";
    case Mnemonic::Rts: return "rts";
    case Mnemonic::Sbcd: return "sbcd";
    case Mnemonic::Scc: return "scc";
    case Mnemonic::Stop: return "stop";
    case Mnemonic::Sub: return "sub";
    case Mnemonic::Suba: return "suba";
    case Mnemonic::Subi: return "subi";
    case Mnemonic::Subq: return "subq";
    case Mnemonic::Subx: return "subx";
    case Mnemonic::Swap: return "swap";
    case Mnemonic::Tas: return "tas";
    case Mnemonic::Trap: return "trap";
    case Mnemonic::Trapv: return "trapv";
    case Mnemonic::Tst: return "tst";
    case Mnemonic::Unlk: return "unlk";
    case Mnemonic::LineA: return "line_a";
    case Mnemonic::LineF: return "line_f";
    }
    return "?";
}

namespace {

constexpr Size size_from_field(u16 f) noexcept {
    return f == 0 ? Size::Byte : (f == 1 ? Size::Word : Size::Long);
}

// MOVE encodes its size differently from every other instruction.
constexpr Size move_size(u16 op) noexcept {
    switch (op >> 12) {
    case 1: return Size::Byte;
    case 3: return Size::Word;
    default: return Size::Long;
    }
}

Operand reg_operand(EAKind kind, u8 reg) noexcept {
    Operand o;
    o.kind = kind;
    o.reg = reg;
    return o;
}

Operand imm_operand(u32 value) noexcept {
    Operand o;
    o.kind = EAKind::Immediate;
    o.value = value;
    return o;
}

} // namespace

u16 ea_cycles(const Operand& op, Size size) noexcept {
    const bool lng = size == Size::Long;
    switch (op.kind) {
    case EAKind::DataReg:
    case EAKind::AddrReg:
    case EAKind::None:
    case EAKind::RegList:
    case EAKind::Ccr:
    case EAKind::Sr:
    case EAKind::Usp:
        return 0;
    case EAKind::Indirect:
    case EAKind::PostInc:
        return lng ? 8 : 4;
    case EAKind::PreDec:
        return lng ? 10 : 6;
    case EAKind::Displacement:
    case EAKind::PcDisplacement:
    case EAKind::AbsW:
        return lng ? 12 : 8;
    case EAKind::IndexIndirect:
    case EAKind::PcIndex:
        return lng ? 14 : 10;
    case EAKind::AbsL:
        return lng ? 16 : 12;
    case EAKind::Immediate:
        return lng ? 8 : 4;
    }
    return 0;
}

Operand Decoder::decode_ea(u32 pc, u8 mode, u8 reg, Size size, u8& offset) const {
    Operand o;
    switch (mode) {
    case 0:
        o.kind = EAKind::DataReg;
        o.reg = reg;
        break;
    case 1:
        o.kind = EAKind::AddrReg;
        o.reg = reg;
        break;
    case 2:
        o.kind = EAKind::Indirect;
        o.reg = reg;
        break;
    case 3:
        o.kind = EAKind::PostInc;
        o.reg = reg;
        break;
    case 4:
        o.kind = EAKind::PreDec;
        o.reg = reg;
        break;
    case 5: {
        o.kind = EAKind::Displacement;
        o.reg = reg;
        o.displacement = static_cast<s16>(word(pc + offset));
        offset += 2;
        break;
    }
    case 6: {
        const u16 ext = word(pc + offset);
        offset += 2;
        o.kind = EAKind::IndexIndirect;
        o.reg = reg;
        o.displacement = static_cast<s8>(ext & 0xff);
        o.index.reg = static_cast<u8>((ext >> 12) & 7);
        o.index.address = (ext & 0x8000) != 0;
        o.index.long_size = (ext & 0x0800) != 0;
        break;
    }
    case 7:
        switch (reg) {
        case 0:
            o.kind = EAKind::AbsW;
            // (xxx).W sign-extends to a full address.
            o.value = sign_extend_16(word(pc + offset));
            offset += 2;
            break;
        case 1:
            o.kind = EAKind::AbsL;
            o.value = longword(pc + offset);
            offset += 4;
            break;
        case 2: {
            const u32 ext_addr = pc + offset;
            o.kind = EAKind::PcDisplacement;
            o.displacement = static_cast<s16>(word(ext_addr));
            // The PC used is the address of the extension word itself.
            o.value = ext_addr + static_cast<u32>(o.displacement);
            offset += 2;
            break;
        }
        case 3: {
            const u32 ext_addr = pc + offset;
            const u16 ext = word(ext_addr);
            offset += 2;
            o.kind = EAKind::PcIndex;
            o.displacement = static_cast<s8>(ext & 0xff);
            o.value = ext_addr;    // base PC for the index calculation
            o.index.reg = static_cast<u8>((ext >> 12) & 7);
            o.index.address = (ext & 0x8000) != 0;
            o.index.long_size = (ext & 0x0800) != 0;
            break;
        }
        case 4:
            o.kind = EAKind::Immediate;
            if (size == Size::Long) {
                o.value = longword(pc + offset);
                offset += 4;
            } else if (size == Size::Byte) {
                o.value = word(pc + offset) & 0xff;
                offset += 2;
            } else {
                o.value = word(pc + offset);
                offset += 2;
            }
            break;
        default:
            o.kind = EAKind::None;   // invalid encoding
            break;
        }
        break;
    default:
        o.kind = EAKind::None;
        break;
    }
    return o;
}

DecodedInstruction Decoder::decode(u32 pc) const {
    DecodedInstruction in;
    in.pc = pc;
    in.opcode = word(pc);
    const u16 op = in.opcode;
    u8 off = 2;

    const u8 mode = static_cast<u8>((op >> 3) & 7);
    const u8 reg = static_cast<u8>(op & 7);
    const u8 reg9 = static_cast<u8>((op >> 9) & 7);
    const u16 opmode = static_cast<u16>((op >> 6) & 7);

    auto finish = [&](Mnemonic m, u32 extra_flags = 0) {
        in.mnemonic = m;
        in.flags |= INSN_VALID | extra_flags;
    };
    auto illegal = [&](Mnemonic m) {
        in.mnemonic = m;
        in.flags |= INSN_ENDS_BLOCK | INSN_TRAP;
    };

    switch (op >> 12) {
    // -----------------------------------------------------------------
    case 0x0: {
        // Immediate to CCR/SR forms come first: they alias EA encodings.
        switch (op) {
        case 0x003c: in.src = imm_operand(word(pc + off) & 0xff); off += 2;
            in.dst = reg_operand(EAKind::Ccr, 0); in.size = Size::Byte;
            in.base_cycles = 20; finish(Mnemonic::OriToCcr); goto done;
        case 0x007c: in.src = imm_operand(word(pc + off)); off += 2;
            in.dst = reg_operand(EAKind::Sr, 0); in.size = Size::Word;
            in.base_cycles = 20; finish(Mnemonic::OriToSr, INSN_PRIVILEGED); goto done;
        case 0x023c: in.src = imm_operand(word(pc + off) & 0xff); off += 2;
            in.dst = reg_operand(EAKind::Ccr, 0); in.size = Size::Byte;
            in.base_cycles = 20; finish(Mnemonic::AndiToCcr); goto done;
        case 0x027c: in.src = imm_operand(word(pc + off)); off += 2;
            in.dst = reg_operand(EAKind::Sr, 0); in.size = Size::Word;
            in.base_cycles = 20; finish(Mnemonic::AndiToSr, INSN_PRIVILEGED); goto done;
        case 0x0a3c: in.src = imm_operand(word(pc + off) & 0xff); off += 2;
            in.dst = reg_operand(EAKind::Ccr, 0); in.size = Size::Byte;
            in.base_cycles = 20; finish(Mnemonic::EoriToCcr); goto done;
        case 0x0a7c: in.src = imm_operand(word(pc + off)); off += 2;
            in.dst = reg_operand(EAKind::Sr, 0); in.size = Size::Word;
            in.base_cycles = 20; finish(Mnemonic::EoriToSr, INSN_PRIVILEGED); goto done;
        default: break;
        }

        // MOVEP: 0000 rrr1 mm 001 aaa. Must be tested before dynamic BTST,
        // whose encoding it otherwise matches.
        if ((op & 0xf138) == 0x0108) {
            const bool to_memory = (op & 0x0080) != 0;
            in.size = (op & 0x0040) ? Size::Long : Size::Word;
            Operand areg = reg_operand(EAKind::Displacement, reg);
            areg.displacement = static_cast<s16>(word(pc + off));
            off += 2;
            if (to_memory) {
                in.src = reg_operand(EAKind::DataReg, reg9);
                in.dst = areg;
            } else {
                in.src = areg;
                in.dst = reg_operand(EAKind::DataReg, reg9);
            }
            in.base_cycles = in.size == Size::Long ? 24 : 16;
            finish(Mnemonic::Movep);
            goto done;
        }

        // Dynamic bit operations: bit number in Dn.
        if (op & 0x0100) {
            static const Mnemonic kBitOps[4] = {Mnemonic::Btst, Mnemonic::Bchg,
                                                Mnemonic::Bclr, Mnemonic::Bset};
            const Mnemonic m = kBitOps[(op >> 6) & 3];
            in.src = reg_operand(EAKind::DataReg, reg9);
            in.size = (mode == 0) ? Size::Long : Size::Byte;
            in.dst = decode_ea(pc, mode, reg, in.size, off);
            in.base_cycles = static_cast<u16>((mode == 0 ? 6 : 8) + ea_cycles(in.dst, Size::Byte));
            finish(m);
            goto done;
        }

        // Static bit operations: bit number is an immediate byte.
        if ((op & 0x0f00) == 0x0800) {
            static const Mnemonic kBitOps[4] = {Mnemonic::Btst, Mnemonic::Bchg,
                                                Mnemonic::Bclr, Mnemonic::Bset};
            const Mnemonic m = kBitOps[(op >> 6) & 3];
            in.src = imm_operand(word(pc + off) & 0xff);
            off += 2;
            in.size = (mode == 0) ? Size::Long : Size::Byte;
            in.dst = decode_ea(pc, mode, reg, in.size, off);
            in.base_cycles = static_cast<u16>((mode == 0 ? 10 : 12) + ea_cycles(in.dst, Size::Byte));
            finish(m);
            goto done;
        }

        // Immediate arithmetic/logic.
        {
            const u16 size_field = (op >> 6) & 3;
            if (size_field == 3) { illegal(Mnemonic::Illegal); goto done; }
            Mnemonic m;
            switch ((op >> 9) & 7) {
            case 0: m = Mnemonic::Ori; break;
            case 1: m = Mnemonic::Andi; break;
            case 2: m = Mnemonic::Subi; break;
            case 3: m = Mnemonic::Addi; break;
            case 5: m = Mnemonic::Eori; break;
            case 6: m = Mnemonic::Cmpi; break;
            default: illegal(Mnemonic::Illegal); goto done;
            }
            in.size = size_from_field(size_field);
            in.src = decode_ea(pc, 7, 4, in.size, off);   // immediate
            in.dst = decode_ea(pc, mode, reg, in.size, off);
            if (in.dst.kind == EAKind::None || in.dst.kind == EAKind::AddrReg) {
                illegal(Mnemonic::Illegal);
                goto done;
            }
            in.base_cycles = static_cast<u16>((in.size == Size::Long ? 16 : 8) +
                                              ea_cycles(in.dst, in.size));
            finish(m);
            goto done;
        }
    }

    // -----------------------------------------------------------------
    case 0x1:
    case 0x2:
    case 0x3: {
        in.size = move_size(op);
        const u8 dst_mode = static_cast<u8>((op >> 6) & 7);
        in.src = decode_ea(pc, mode, reg, in.size, off);
        in.dst = decode_ea(pc, dst_mode, reg9, in.size, off);
        if (in.src.kind == EAKind::None || in.dst.kind == EAKind::None ||
            (in.size == Size::Byte && (in.src.kind == EAKind::AddrReg ||
                                       in.dst.kind == EAKind::AddrReg))) {
            illegal(Mnemonic::Illegal);
            goto done;
        }
        in.base_cycles = static_cast<u16>(4 + ea_cycles(in.src, in.size) +
                                          ea_cycles(in.dst, in.size));
        finish(dst_mode == 1 ? Mnemonic::Movea : Mnemonic::Move);
        goto done;
    }

    // -----------------------------------------------------------------
    case 0x4: {
        if (op == 0x4afc) { illegal(Mnemonic::IllegalOp); goto done; }

        if ((op & 0xf1c0) == 0x41c0) {   // LEA
            in.size = Size::Long;
            in.src = decode_ea(pc, mode, reg, Size::Long, off);
            in.dst = reg_operand(EAKind::AddrReg, reg9);
            if (!in.src.is_memory() || in.src.kind == EAKind::PostInc ||
                in.src.kind == EAKind::PreDec) {
                illegal(Mnemonic::Illegal);
                goto done;
            }
            in.base_cycles = static_cast<u16>(4 + ea_cycles(in.src, Size::Long));
            finish(Mnemonic::Lea);
            goto done;
        }
        if ((op & 0xf1c0) == 0x4180) {   // CHK.W
            in.size = Size::Word;
            in.src = decode_ea(pc, mode, reg, Size::Word, off);
            in.dst = reg_operand(EAKind::DataReg, reg9);
            in.base_cycles = static_cast<u16>(10 + ea_cycles(in.src, Size::Word));
            finish(Mnemonic::Chk, INSN_TRAP);
            goto done;
        }

        switch (op & 0xffc0) {
        case 0x40c0:
            in.size = Size::Word;
            in.src = reg_operand(EAKind::Sr, 0);
            in.dst = decode_ea(pc, mode, reg, Size::Word, off);
            in.base_cycles = static_cast<u16>(6 + ea_cycles(in.dst, Size::Word));
            finish(Mnemonic::MoveFromSr);
            goto done;
        case 0x44c0:
            in.size = Size::Word;
            in.src = decode_ea(pc, mode, reg, Size::Word, off);
            in.dst = reg_operand(EAKind::Ccr, 0);
            in.base_cycles = static_cast<u16>(12 + ea_cycles(in.src, Size::Word));
            finish(Mnemonic::MoveToCcr);
            goto done;
        case 0x46c0:
            in.size = Size::Word;
            in.src = decode_ea(pc, mode, reg, Size::Word, off);
            in.dst = reg_operand(EAKind::Sr, 0);
            in.base_cycles = static_cast<u16>(12 + ea_cycles(in.src, Size::Word));
            finish(Mnemonic::MoveToSr, INSN_PRIVILEGED);
            goto done;
        case 0x4800:
            in.size = Size::Byte;
            in.dst = decode_ea(pc, mode, reg, Size::Byte, off);
            in.base_cycles = static_cast<u16>(6 + ea_cycles(in.dst, Size::Byte));
            finish(Mnemonic::Nbcd);
            goto done;
        case 0x4ac0:
            in.size = Size::Byte;
            in.dst = decode_ea(pc, mode, reg, Size::Byte, off);
            in.base_cycles = static_cast<u16>(4 + ea_cycles(in.dst, Size::Byte));
            finish(Mnemonic::Tas);
            goto done;
        case 0x4e80:
            in.size = Size::Long;
            in.src = decode_ea(pc, mode, reg, Size::Long, off);
            in.base_cycles = static_cast<u16>(12 + ea_cycles(in.src, Size::Long));
            in.flags |= INSN_CALL | INSN_ENDS_BLOCK;
            if (in.src.is_static_address()) in.target = in.src.value;
            else in.flags |= INSN_INDIRECT;
            finish(Mnemonic::Jsr);
            goto done;
        case 0x4ec0:
            in.size = Size::Long;
            in.src = decode_ea(pc, mode, reg, Size::Long, off);
            in.base_cycles = static_cast<u16>(4 + ea_cycles(in.src, Size::Long));
            in.flags |= INSN_ENDS_BLOCK;
            if (in.src.is_static_address()) in.target = in.src.value;
            else in.flags |= INSN_INDIRECT;
            finish(Mnemonic::Jmp);
            goto done;
        default: break;
        }

        // SWAP / PEA / EXT / MOVEM share the 0x48xx quarter.
        if ((op & 0xfff8) == 0x4840) {
            in.size = Size::Long;
            in.dst = reg_operand(EAKind::DataReg, reg);
            in.base_cycles = 4;
            finish(Mnemonic::Swap);
            goto done;
        }
        if ((op & 0xffc0) == 0x4840) {
            in.size = Size::Long;
            in.src = decode_ea(pc, mode, reg, Size::Long, off);
            in.base_cycles = static_cast<u16>(4 + ea_cycles(in.src, Size::Long));
            finish(Mnemonic::Pea);
            goto done;
        }
        if ((op & 0xfeb8) == 0x4880 && mode == 0) {   // EXT.W / EXT.L
            in.size = (op & 0x0040) ? Size::Long : Size::Word;
            in.dst = reg_operand(EAKind::DataReg, reg);
            in.base_cycles = 4;
            finish(Mnemonic::Ext);
            goto done;
        }
        if ((op & 0xfb80) == 0x4880) {   // MOVEM
            const bool to_registers = (op & 0x0400) != 0;
            in.size = (op & 0x0040) ? Size::Long : Size::Word;
            Operand list;
            list.kind = EAKind::RegList;
            list.value = word(pc + off);
            off += 2;
            Operand ea = decode_ea(pc, mode, reg, in.size, off);
            if (to_registers) {
                in.src = ea;
                in.dst = list;
            } else {
                in.src = list;
                in.dst = ea;
            }
            in.base_cycles = static_cast<u16>(to_registers ? 12 : 8);
            finish(Mnemonic::Movem);
            goto done;
        }

        // NEGX / CLR / NEG / NOT / TST share bits 11-9 with a size field.
        {
            const u16 size_field = (op >> 6) & 3;
            if (size_field != 3) {
                Mnemonic m;
                bool ok = true;
                switch ((op >> 9) & 7) {
                case 0: m = Mnemonic::Negx; break;
                case 1: m = Mnemonic::Clr; break;
                case 2: m = Mnemonic::Neg; break;
                case 3: m = Mnemonic::Not; break;
                case 5: m = Mnemonic::Tst; break;
                default: m = Mnemonic::Illegal; ok = false; break;
                }
                if (ok) {
                    in.size = size_from_field(size_field);
                    Operand ea = decode_ea(pc, mode, reg, in.size, off);
                    if (m == Mnemonic::Tst) in.src = ea; else in.dst = ea;
                    in.base_cycles = static_cast<u16>(4 + ea_cycles(ea, in.size));
                    finish(m);
                    goto done;
                }
            }
        }

        // Fixed-encoding control instructions.
        if ((op & 0xfff0) == 0x4e40) {
            in.src = imm_operand(op & 0x0f);
            in.base_cycles = 34;
            in.flags |= INSN_ENDS_BLOCK | INSN_TRAP;
            finish(Mnemonic::Trap);
            goto done;
        }
        if ((op & 0xfff8) == 0x4e50) {
            in.dst = reg_operand(EAKind::AddrReg, reg);
            in.src = imm_operand(word(pc + off));
            off += 2;
            in.size = Size::Word;
            in.base_cycles = 16;
            finish(Mnemonic::Link);
            goto done;
        }
        if ((op & 0xfff8) == 0x4e58) {
            in.dst = reg_operand(EAKind::AddrReg, reg);
            in.base_cycles = 12;
            finish(Mnemonic::Unlk);
            goto done;
        }
        if ((op & 0xfff8) == 0x4e60) {
            in.src = reg_operand(EAKind::AddrReg, reg);
            in.dst = reg_operand(EAKind::Usp, 0);
            in.base_cycles = 4;
            finish(Mnemonic::MoveAnToUsp, INSN_PRIVILEGED);
            goto done;
        }
        if ((op & 0xfff8) == 0x4e68) {
            in.src = reg_operand(EAKind::Usp, 0);
            in.dst = reg_operand(EAKind::AddrReg, reg);
            in.base_cycles = 4;
            finish(Mnemonic::MoveUspToAn, INSN_PRIVILEGED);
            goto done;
        }
        switch (op) {
        case 0x4e70: in.base_cycles = 132; finish(Mnemonic::Reset, INSN_PRIVILEGED); goto done;
        case 0x4e71: in.base_cycles = 4; finish(Mnemonic::Nop); goto done;
        case 0x4e72:
            in.src = imm_operand(word(pc + off));
            off += 2;
            in.base_cycles = 4;
            finish(Mnemonic::Stop, INSN_PRIVILEGED | INSN_ENDS_BLOCK);
            goto done;
        case 0x4e73: in.base_cycles = 20;
            finish(Mnemonic::Rte, INSN_PRIVILEGED | INSN_ENDS_BLOCK | INSN_RETURN | INSN_INDIRECT);
            goto done;
        case 0x4e75: in.base_cycles = 16;
            finish(Mnemonic::Rts, INSN_ENDS_BLOCK | INSN_RETURN | INSN_INDIRECT);
            goto done;
        case 0x4e76: in.base_cycles = 4; finish(Mnemonic::Trapv, INSN_TRAP); goto done;
        case 0x4e77: in.base_cycles = 20;
            finish(Mnemonic::Rtr, INSN_ENDS_BLOCK | INSN_RETURN | INSN_INDIRECT);
            goto done;
        default: break;
        }
        illegal(Mnemonic::Illegal);
        goto done;
    }

    // -----------------------------------------------------------------
    case 0x5: {
        if ((op & 0x00c0) == 0x00c0) {
            if ((op & 0xf0f8) == 0x50c8) {   // DBcc
                in.condition = static_cast<Cond>((op >> 8) & 15);
                in.dst = reg_operand(EAKind::DataReg, reg);
                const s32 disp = static_cast<s16>(word(pc + off));
                off += 2;
                in.target = static_cast<u32>(pc + 2 + disp);
                in.size = Size::Word;
                in.base_cycles = 10;
                finish(Mnemonic::Dbcc, INSN_CONDITIONAL | INSN_ENDS_BLOCK);
                goto done;
            }
            // Scc
            in.condition = static_cast<Cond>((op >> 8) & 15);
            in.size = Size::Byte;
            in.dst = decode_ea(pc, mode, reg, Size::Byte, off);
            in.base_cycles = static_cast<u16>((mode == 0 ? 4 : 8) + ea_cycles(in.dst, Size::Byte));
            finish(Mnemonic::Scc);
            goto done;
        }
        // ADDQ / SUBQ
        in.size = size_from_field((op >> 6) & 3);
        u32 count = reg9 == 0 ? 8u : reg9;
        in.src = imm_operand(count);
        in.dst = decode_ea(pc, mode, reg, in.size, off);
        if (in.dst.kind == EAKind::AddrReg && in.size == Size::Byte) {
            illegal(Mnemonic::Illegal);
            goto done;
        }
        in.base_cycles = static_cast<u16>((in.size == Size::Long ? 8 : 4) +
                                          ea_cycles(in.dst, in.size));
        finish((op & 0x0100) ? Mnemonic::Subq : Mnemonic::Addq);
        goto done;
    }

    // -----------------------------------------------------------------
    case 0x6: {
        const u16 cond = (op >> 8) & 15;
        s32 disp = static_cast<s8>(op & 0xff);
        in.size = Size::Byte;
        if ((op & 0xff) == 0x00) {
            // Word displacement form. The 68000 has no long form: 0xff is an
            // ordinary displacement of -1, not a size escape.
            disp = static_cast<s16>(word(pc + off));
            off += 2;
            in.size = Size::Word;
        }
        in.target = static_cast<u32>(pc + 2 + disp);
        in.condition = static_cast<Cond>(cond);
        if (cond == 0) {
            in.base_cycles = 10;
            finish(Mnemonic::Bra, INSN_ENDS_BLOCK);
        } else if (cond == 1) {
            in.base_cycles = 18;
            finish(Mnemonic::Bsr, INSN_ENDS_BLOCK | INSN_CALL);
        } else {
            in.base_cycles = 10;
            finish(Mnemonic::Bcc, INSN_ENDS_BLOCK | INSN_CONDITIONAL);
        }
        goto done;
    }

    // -----------------------------------------------------------------
    case 0x7: {
        if (op & 0x0100) { illegal(Mnemonic::Illegal); goto done; }
        in.size = Size::Long;
        in.src = imm_operand(sign_extend_8(static_cast<u8>(op & 0xff)));
        in.dst = reg_operand(EAKind::DataReg, reg9);
        in.base_cycles = 4;
        finish(Mnemonic::Moveq);
        goto done;
    }

    // -----------------------------------------------------------------
    case 0x8:
    case 0x9:
    case 0xb:
    case 0xc:
    case 0xd: {
        const u16 group = op >> 12;

        // SBCD / ABCD / SUBX / ADDX / CMPM share the "register or memory"
        // sub-encoding with bit 8 set.
        if ((group == 0x8 || group == 0xc) && (op & 0x01f0) == 0x0100) {
            const bool memory = (op & 0x0008) != 0;
            in.size = Size::Byte;
            in.src = reg_operand(memory ? EAKind::PreDec : EAKind::DataReg, reg);
            in.dst = reg_operand(memory ? EAKind::PreDec : EAKind::DataReg, reg9);
            in.base_cycles = memory ? 18 : 6;
            finish(group == 0x8 ? Mnemonic::Sbcd : Mnemonic::Abcd);
            goto done;
        }
        if ((group == 0x9 || group == 0xd) && (op & 0x0130) == 0x0100 &&
            ((op >> 6) & 3) != 3) {
            const bool memory = (op & 0x0008) != 0;
            in.size = size_from_field((op >> 6) & 3);
            in.src = reg_operand(memory ? EAKind::PreDec : EAKind::DataReg, reg);
            in.dst = reg_operand(memory ? EAKind::PreDec : EAKind::DataReg, reg9);
            in.base_cycles = static_cast<u16>(memory ? (in.size == Size::Long ? 30 : 18)
                                                     : (in.size == Size::Long ? 8 : 4));
            finish(group == 0x9 ? Mnemonic::Subx : Mnemonic::Addx);
            goto done;
        }
        if (group == 0xb && (op & 0x0138) == 0x0108) {   // CMPM
            in.size = size_from_field((op >> 6) & 3);
            in.src = reg_operand(EAKind::PostInc, reg);
            in.dst = reg_operand(EAKind::PostInc, reg9);
            in.base_cycles = static_cast<u16>(in.size == Size::Long ? 20 : 12);
            finish(Mnemonic::Cmpm);
            goto done;
        }
        if (group == 0xc) {   // EXG
            if ((op & 0xf1f8) == 0xc140) {
                in.src = reg_operand(EAKind::DataReg, reg9);
                in.dst = reg_operand(EAKind::DataReg, reg);
                in.size = Size::Long;
                in.base_cycles = 6;
                finish(Mnemonic::Exg);
                goto done;
            }
            if ((op & 0xf1f8) == 0xc148) {
                in.src = reg_operand(EAKind::AddrReg, reg9);
                in.dst = reg_operand(EAKind::AddrReg, reg);
                in.size = Size::Long;
                in.base_cycles = 6;
                finish(Mnemonic::Exg);
                goto done;
            }
            if ((op & 0xf1f8) == 0xc188) {
                in.src = reg_operand(EAKind::DataReg, reg9);
                in.dst = reg_operand(EAKind::AddrReg, reg);
                in.size = Size::Long;
                in.base_cycles = 6;
                finish(Mnemonic::Exg);
                goto done;
            }
        }

        // MULU/MULS/DIVU/DIVS occupy opmode 011 and 111 of groups 8 and c.
        if (opmode == 3 || opmode == 7) {
            if (group == 0x8 || group == 0xc) {
                in.size = Size::Word;
                in.src = decode_ea(pc, mode, reg, Size::Word, off);
                in.dst = reg_operand(EAKind::DataReg, reg9);
                if (in.src.kind == EAKind::AddrReg || in.src.kind == EAKind::None) {
                    illegal(Mnemonic::Illegal);
                    goto done;
                }
                Mnemonic m;
                if (group == 0x8) {
                    m = (opmode == 3) ? Mnemonic::Divu : Mnemonic::Divs;
                    in.base_cycles = static_cast<u16>((opmode == 3 ? 140 : 158) +
                                                      ea_cycles(in.src, Size::Word));
                    in.flags |= INSN_TRAP;   // divide by zero
                } else {
                    m = (opmode == 3) ? Mnemonic::Mulu : Mnemonic::Muls;
                    in.base_cycles = static_cast<u16>(70 + ea_cycles(in.src, Size::Word));
                }
                finish(m);
                goto done;
            }
            // Groups 9, b, d: ADDA/SUBA/CMPA.
            in.size = (opmode == 3) ? Size::Word : Size::Long;
            in.src = decode_ea(pc, mode, reg, in.size, off);
            in.dst = reg_operand(EAKind::AddrReg, reg9);
            in.base_cycles = static_cast<u16>(8 + ea_cycles(in.src, in.size));
            finish(group == 0x9 ? Mnemonic::Suba
                                : (group == 0xb ? Mnemonic::Cmpa : Mnemonic::Adda));
            goto done;
        }

        // The ordinary <ea>,Dn and Dn,<ea> forms.
        {
            const bool to_ea = (opmode & 4) != 0;
            in.size = size_from_field(opmode & 3);
            Operand ea = decode_ea(pc, mode, reg, in.size, off);
            if (ea.kind == EAKind::None) { illegal(Mnemonic::Illegal); goto done; }

            Mnemonic m;
            switch (group) {
            case 0x8: m = Mnemonic::Or; break;
            case 0x9: m = Mnemonic::Sub; break;
            case 0xb: m = to_ea ? Mnemonic::Eor : Mnemonic::Cmp; break;
            case 0xc: m = Mnemonic::And; break;
            default:  m = Mnemonic::Add; break;
            }

            // Byte operations cannot name an address register, and the
            // Dn,<ea> direction cannot target a register at all.
            if (ea.kind == EAKind::AddrReg &&
                (in.size == Size::Byte || m == Mnemonic::Or || m == Mnemonic::And ||
                 m == Mnemonic::Eor)) {
                illegal(Mnemonic::Illegal);
                goto done;
            }

            if (to_ea) {
                in.src = reg_operand(EAKind::DataReg, reg9);
                in.dst = ea;
                if (!ea.is_memory() && m != Mnemonic::Eor) {
                    illegal(Mnemonic::Illegal);
                    goto done;
                }
                in.base_cycles = static_cast<u16>((in.size == Size::Long ? 12 : 8) +
                                                  ea_cycles(ea, in.size));
            } else {
                in.src = ea;
                in.dst = reg_operand(EAKind::DataReg, reg9);
                in.base_cycles = static_cast<u16>((in.size == Size::Long ? 6 : 4) +
                                                  ea_cycles(ea, in.size));
            }
            finish(m);
            goto done;
        }
    }

    // -----------------------------------------------------------------
    case 0xa:
        illegal(Mnemonic::LineA);
        goto done;

    // -----------------------------------------------------------------
    case 0xe: {
        static const Mnemonic kLeft[4] = {Mnemonic::Asl, Mnemonic::Lsl,
                                          Mnemonic::Roxl, Mnemonic::Rol};
        static const Mnemonic kRight[4] = {Mnemonic::Asr, Mnemonic::Lsr,
                                           Mnemonic::Roxr, Mnemonic::Ror};
        const bool left = (op & 0x0100) != 0;
        if (((op >> 6) & 3) == 3) {
            // Memory form: always word sized, always shifts by one.
            const u16 type = (op >> 9) & 3;
            if ((op >> 11) & 1) { illegal(Mnemonic::Illegal); goto done; }
            in.size = Size::Word;
            in.src = imm_operand(1);
            in.dst = decode_ea(pc, mode, reg, Size::Word, off);
            if (!in.dst.is_memory()) { illegal(Mnemonic::Illegal); goto done; }
            in.base_cycles = static_cast<u16>(8 + ea_cycles(in.dst, Size::Word));
            finish(left ? kLeft[type] : kRight[type]);
            goto done;
        }
        in.size = size_from_field((op >> 6) & 3);
        {
            const u16 type = (op >> 3) & 3;
            const bool count_in_register = (op & 0x0020) != 0;
            if (count_in_register) {
                in.src = reg_operand(EAKind::DataReg, reg9);
            } else {
                in.src = imm_operand(reg9 == 0 ? 8u : reg9);
            }
            in.dst = reg_operand(EAKind::DataReg, reg);
            in.base_cycles = static_cast<u16>(in.size == Size::Long ? 8 : 6);
            finish(left ? kLeft[type] : kRight[type]);
            goto done;
        }
    }

    // -----------------------------------------------------------------
    case 0xf:
    default:
        illegal(Mnemonic::LineF);
        goto done;
    }

done:
    in.length = off;
    if (!in.valid() && in.base_cycles == 4) in.base_cycles = 34;  // exception entry
    return in;
}

// ---------------------------------------------------------------------------

namespace {
struct BufferContext {
    const u8* data;
    std::size_t size;
    u32 base;
};

u16 buffer_fetch(void* ctx, u32 address) {
    auto* c = static_cast<BufferContext*>(ctx);
    const u32 off = address - c->base;
    if (off + 1 >= c->size) return 0xffff;
    return static_cast<u16>((static_cast<u16>(c->data[off]) << 8) | c->data[off + 1]);
}
} // namespace

DecodedInstruction decode_buffer(const u8* data, std::size_t size, u32 base, u32 pc) {
    BufferContext ctx{data, size, base};
    Decoder decoder(&buffer_fetch, &ctx);
    return decoder.decode(pc);
}

// ---------------------------------------------------------------------------

namespace {

std::string format_operand(const Operand& o, Size size) {
    char buf[64];
    switch (o.kind) {
    case EAKind::None: return {};
    case EAKind::DataReg: std::snprintf(buf, sizeof buf, "d%u", o.reg); break;
    case EAKind::AddrReg: std::snprintf(buf, sizeof buf, "a%u", o.reg); break;
    case EAKind::Indirect: std::snprintf(buf, sizeof buf, "(a%u)", o.reg); break;
    case EAKind::PostInc: std::snprintf(buf, sizeof buf, "(a%u)+", o.reg); break;
    case EAKind::PreDec: std::snprintf(buf, sizeof buf, "-(a%u)", o.reg); break;
    case EAKind::Displacement:
        std::snprintf(buf, sizeof buf, "%d(a%u)", o.displacement, o.reg);
        break;
    case EAKind::IndexIndirect:
        std::snprintf(buf, sizeof buf, "%d(a%u,%c%u.%c)", o.displacement, o.reg,
                      o.index.address ? 'a' : 'd', o.index.reg,
                      o.index.long_size ? 'l' : 'w');
        break;
    case EAKind::AbsW: std::snprintf(buf, sizeof buf, "$%08x.w", o.value); break;
    case EAKind::AbsL: std::snprintf(buf, sizeof buf, "$%08x.l", o.value); break;
    case EAKind::PcDisplacement:
        std::snprintf(buf, sizeof buf, "$%08x(pc)", o.value);
        break;
    case EAKind::PcIndex:
        std::snprintf(buf, sizeof buf, "%d(pc,%c%u.%c)", o.displacement,
                      o.index.address ? 'a' : 'd', o.index.reg,
                      o.index.long_size ? 'l' : 'w');
        break;
    case EAKind::Immediate: std::snprintf(buf, sizeof buf, "#$%x", o.value); break;
    case EAKind::RegList: std::snprintf(buf, sizeof buf, "regs:$%04x", o.value); break;
    case EAKind::Ccr: return "ccr";
    case EAKind::Sr: return "sr";
    case EAKind::Usp: return "usp";
    }
    (void)size;
    return buf;
}

} // namespace

std::string disassemble(const DecodedInstruction& in) {
    std::string out = mnemonic_name(in.mnemonic);
    if (in.mnemonic == Mnemonic::Bcc || in.mnemonic == Mnemonic::Scc ||
        in.mnemonic == Mnemonic::Dbcc) {
        out += cond_name(in.condition);
    }
    switch (in.mnemonic) {
    case Mnemonic::Rts: case Mnemonic::Rte: case Mnemonic::Rtr:
    case Mnemonic::Nop: case Mnemonic::Reset: case Mnemonic::Trapv:
    case Mnemonic::Trap: case Mnemonic::Stop: case Mnemonic::IllegalOp:
    case Mnemonic::Illegal: case Mnemonic::LineA: case Mnemonic::LineF:
    case Mnemonic::Unlk: case Mnemonic::Swap: case Mnemonic::Exg:
        break;
    default:
        switch (in.size) {
        case Size::Byte: out += ".b"; break;
        case Size::Word: out += ".w"; break;
        case Size::Long: out += ".l"; break;
        }
        break;
    }

    if (in.mnemonic == Mnemonic::Bcc || in.mnemonic == Mnemonic::Bra ||
        in.mnemonic == Mnemonic::Bsr) {
        char buf[32];
        std::snprintf(buf, sizeof buf, " $%08x", in.target);
        return out + buf;
    }

    const std::string s = format_operand(in.src, in.size);
    const std::string d = format_operand(in.dst, in.size);
    if (!s.empty()) out += " " + s;
    if (!d.empty()) out += (s.empty() ? " " : ",") + d;
    if (in.mnemonic == Mnemonic::Dbcc) {
        char buf[32];
        std::snprintf(buf, sizeof buf, ",$%08x", in.target);
        out += buf;
    }
    return out;
}

} // namespace arecomp
