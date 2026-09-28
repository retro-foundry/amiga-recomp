// MC68000 instruction decoder (AMIGA_RECOMP.md 9, 10).
//
// Produces a typed structure, never text that is later re-parsed. Both halves
// of the project decode through this: the recompiler at build time and the
// reference interpreter at run time.
#pragma once

#include <string>

#include "amiga_recomp/cpu_state.hpp"
#include "amiga_recomp/types.hpp"

namespace arecomp {

enum class Mnemonic : u8 {
    Illegal, Abcd, Add, Adda, Addi, Addq, Addx, And, Andi, AndiToCcr, AndiToSr,
    Asl, Asr, Bcc, Bchg, Bclr, Bra, Bset, Bsr, Btst, Chk, Clr, Cmp, Cmpa, Cmpi,
    Cmpm, Dbcc, Divs, Divu, Eor, Eori, EoriToCcr, EoriToSr, Exg, Ext, IllegalOp,
    Jmp, Jsr, Lea, Link, Lsl, Lsr, Move, Movea, MoveFromSr, MoveToCcr, MoveToSr,
    MoveUspToAn, MoveAnToUsp, Movem, Movep, Moveq, Muls, Mulu, Nbcd, Neg, Negx,
    Nop, Not, Or, Ori, OriToCcr, OriToSr, Pea, Reset, Rol, Ror, Roxl, Roxr, Rte,
    Rtr, Rts, Sbcd, Scc, Stop, Sub, Suba, Subi, Subq, Subx, Swap, Tas, Trap,
    Trapv, Tst, Unlk, LineA, LineF,
};

const char* mnemonic_name(Mnemonic m) noexcept;

enum class EAKind : u8 {
    None,
    DataReg,         // Dn
    AddrReg,         // An
    Indirect,        // (An)
    PostInc,         // (An)+
    PreDec,          // -(An)
    Displacement,    // d16(An)
    IndexIndirect,   // d8(An,Xn)
    AbsW,            // (xxx).W
    AbsL,            // (xxx).L
    PcDisplacement,  // d16(PC)
    PcIndex,         // d8(PC,Xn)
    Immediate,       // #imm
    RegList,         // MOVEM register mask
    Ccr,             // implicit CCR
    Sr,              // implicit SR
    Usp,             // implicit USP
};

struct IndexSpec {
    u8 reg = 0;            // Xn register number
    bool address = false;  // true: An, false: Dn
    bool long_size = false;// true: full 32-bit index, false: sign-extended word
};

struct Operand {
    EAKind kind = EAKind::None;
    u8 reg = 0;          // register number for register-bearing modes
    s32 displacement = 0;// d16 / d8
    u32 value = 0;       // absolute address, immediate, or MOVEM register mask
    IndexSpec index;

    [[nodiscard]] bool is_register() const noexcept {
        return kind == EAKind::DataReg || kind == EAKind::AddrReg;
    }
    // True for modes that read or write guest memory.
    [[nodiscard]] bool is_memory() const noexcept {
        switch (kind) {
        case EAKind::Indirect: case EAKind::PostInc: case EAKind::PreDec:
        case EAKind::Displacement: case EAKind::IndexIndirect:
        case EAKind::AbsW: case EAKind::AbsL:
        case EAKind::PcDisplacement: case EAKind::PcIndex:
            return true;
        default:
            return false;
        }
    }
    // True for modes whose address depends only on the instruction, not on
    // register contents: the recompiler can fold these to a constant.
    [[nodiscard]] bool is_static_address() const noexcept {
        return kind == EAKind::AbsW || kind == EAKind::AbsL ||
               kind == EAKind::PcDisplacement;
    }
};

enum : u32 {
    INSN_ENDS_BLOCK = 1u << 0,   // unconditional transfer or return
    INSN_CONDITIONAL = 1u << 1,  // conditional transfer: two successors
    INSN_CALL = 1u << 2,         // BSR / JSR
    INSN_INDIRECT = 1u << 3,     // target is not statically known
    INSN_PRIVILEGED = 1u << 4,
    INSN_RETURN = 1u << 5,       // RTS / RTE / RTR
    INSN_TRAP = 1u << 6,
    INSN_VALID = 1u << 7,        // decoded to a real instruction
};

struct DecodedInstruction {
    u32 pc = 0;
    u16 opcode = 0;
    Mnemonic mnemonic = Mnemonic::Illegal;
    Operand src;
    Operand dst;
    Size size = Size::Word;
    u8 length = 2;          // total bytes, including extension words
    u16 base_cycles = 4;
    u32 flags = 0;
    Cond condition = Cond::T;  // for Bcc / Scc / DBcc
    u32 target = 0;            // resolved branch target, when statically known

    [[nodiscard]] bool valid() const noexcept { return (flags & INSN_VALID) != 0; }
    [[nodiscard]] u32 next_pc() const noexcept { return pc + length; }
};

// Reads instruction words through `fetch`, which must return the big-endian
// word at a guest address. Never throws for an undecodable opcode: it returns
// an Illegal/LineA/LineF instruction so the caller can route it to the correct
// guest exception vector (rule 9).
class Decoder {
public:
    using FetchFn = u16 (*)(void* context, u32 address);

    Decoder(FetchFn fetch, void* context) : fetch_(fetch), context_(context) {}

    DecodedInstruction decode(u32 pc) const;

private:
    [[nodiscard]] u16 word(u32 a) const { return fetch_(context_, a); }
    [[nodiscard]] u32 longword(u32 a) const {
        return (static_cast<u32>(word(a)) << 16) | word(a + 2);
    }

    // Decodes the extension words for a mode/register pair. `offset` is the
    // byte offset of the first extension word from the opcode; it is advanced
    // past whatever this mode consumes.
    Operand decode_ea(u32 pc, u8 mode, u8 reg, Size size, u8& offset) const;

    FetchFn fetch_;
    void* context_;
};

// Convenience: decode straight out of a flat buffer.
DecodedInstruction decode_buffer(const u8* data, std::size_t size, u32 base,
                                 u32 pc);

// Human-readable form, used for the comments in generated code and by the
// debugger. Never parsed back.
std::string disassemble(const DecodedInstruction& insn);

// Extra cycles for an effective address, added to an instruction's base cost.
u16 ea_cycles(const Operand& op, Size size) noexcept;

} // namespace arecomp
