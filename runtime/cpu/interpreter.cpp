// Reference MC68000 interpreter (AMIGA_RECOMP.md 27, 46).
//
// Two jobs, neither of which is "run the game":
//   1. the development fallback when the dispatcher reaches an address with no
//      compiled block, so discovery can converge (26.2);
//   2. the oracle the generated code is differentially tested against (28).
//
// It shares semantics.hpp with the translator, so it validates *translation*
// -- decode, effective addresses, control flow, block stitching -- and not the
// shared flag formulas themselves. Those are covered by the hand-written unit
// tests and, when ARECOMP_WITH_MUSASHI is enabled, by an external oracle.
// docs/architecture.md says so plainly rather than overclaiming.
//
// A shipping port compiles this out entirely (rule 18).

#include "amiga_recomp/decoder.hpp"
#include "amiga_recomp/interpreter.hpp"
#include "amiga_recomp/runtime.hpp"
#include "amiga_recomp/semantics.hpp"

namespace arecomp {

namespace {

u16 runtime_fetch(void* ctx, u32 address) {
    return static_cast<Runtime*>(ctx)->fetch16(address);
}

// A resolved effective address. Register side effects (predecrement,
// postincrement) have already happened exactly once by the time this exists.
struct Resolved {
    enum class Loc : u8 { None, Dn, An, Mem, Imm, Ccr, Sr, Usp } loc = Loc::None;
    u8 reg = 0;
    u32 addr = 0;
    u32 imm = 0;
};

// On the 68000, byte accesses through A7 still move the stack pointer by two,
// so the stack stays word aligned. One place, tested explicitly.
u32 pointer_step(const Operand& o, Size size) {
    const u32 step = size_bytes(size);
    if (size == Size::Byte && o.reg == 7) return 2;
    return step;
}

u32 index_value(const IndexSpec& idx, const M68kState& cpu) {
    const u32 raw = idx.address ? cpu.a[idx.reg] : cpu.d[idx.reg];
    return idx.long_size ? raw : sign_extend_16(static_cast<u16>(raw));
}

Resolved resolve(const Operand& o, Size size, M68kState& cpu) {
    Resolved r;
    switch (o.kind) {
    case EAKind::DataReg: r.loc = Resolved::Loc::Dn; r.reg = o.reg; break;
    case EAKind::AddrReg: r.loc = Resolved::Loc::An; r.reg = o.reg; break;
    case EAKind::Indirect:
        r.loc = Resolved::Loc::Mem;
        r.addr = cpu.a[o.reg];
        break;
    case EAKind::PostInc:
        r.loc = Resolved::Loc::Mem;
        r.addr = cpu.a[o.reg];
        cpu.a[o.reg] += pointer_step(o, size);
        break;
    case EAKind::PreDec:
        cpu.a[o.reg] -= pointer_step(o, size);
        r.loc = Resolved::Loc::Mem;
        r.addr = cpu.a[o.reg];
        break;
    case EAKind::Displacement:
        r.loc = Resolved::Loc::Mem;
        r.addr = cpu.a[o.reg] + static_cast<u32>(o.displacement);
        break;
    case EAKind::IndexIndirect:
        r.loc = Resolved::Loc::Mem;
        r.addr = cpu.a[o.reg] + static_cast<u32>(o.displacement) +
                 index_value(o.index, cpu);
        break;
    case EAKind::AbsW:
    case EAKind::AbsL:
    case EAKind::PcDisplacement:
        r.loc = Resolved::Loc::Mem;
        r.addr = o.value;
        break;
    case EAKind::PcIndex:
        r.loc = Resolved::Loc::Mem;
        r.addr = o.value + static_cast<u32>(o.displacement) + index_value(o.index, cpu);
        break;
    case EAKind::Immediate:
        r.loc = Resolved::Loc::Imm;
        r.imm = o.value;
        break;
    case EAKind::Ccr: r.loc = Resolved::Loc::Ccr; break;
    case EAKind::Sr: r.loc = Resolved::Loc::Sr; break;
    case EAKind::Usp: r.loc = Resolved::Loc::Usp; break;
    default: break;
    }
    return r;
}

u32 read_resolved(const Resolved& r, Size size, M68kState& cpu, Runtime& rt) {
    switch (r.loc) {
    case Resolved::Loc::Dn:
        switch (size) {
        case Size::Byte: return cpu.d[r.reg] & 0xff;
        case Size::Word: return cpu.d[r.reg] & 0xffff;
        default: return cpu.d[r.reg];
        }
    case Resolved::Loc::An:
        return size == Size::Word ? (cpu.a[r.reg] & 0xffff) : cpu.a[r.reg];
    case Resolved::Loc::Mem:
        switch (size) {
        case Size::Byte: return rt.read8(r.addr);
        case Size::Word: return rt.read16(r.addr);
        default: return rt.read32(r.addr);
        }
    case Resolved::Loc::Imm: return r.imm;
    case Resolved::Loc::Ccr: return cpu.ccr();
    case Resolved::Loc::Sr: return cpu.sr();
    case Resolved::Loc::Usp: return cpu.inactive_sp();
    default: return 0;
    }
}

void write_resolved(const Resolved& r, Size size, u32 value, M68kState& cpu,
                    Runtime& rt) {
    switch (r.loc) {
    case Resolved::Loc::Dn:
        switch (size) {
        case Size::Byte: cpu.d[r.reg] = replace_lo8(cpu.d[r.reg], static_cast<u8>(value)); break;
        case Size::Word: cpu.d[r.reg] = replace_lo16(cpu.d[r.reg], static_cast<u16>(value)); break;
        default: cpu.d[r.reg] = value; break;
        }
        break;
    case Resolved::Loc::An:
        // Writes to an address register are always full width, sign-extended
        // from a word.
        cpu.a[r.reg] = size == Size::Word ? sign_extend_16(static_cast<u16>(value)) : value;
        break;
    case Resolved::Loc::Mem:
        switch (size) {
        case Size::Byte: rt.write8(r.addr, static_cast<u8>(value)); break;
        case Size::Word: rt.write16(r.addr, static_cast<u16>(value)); break;
        default: rt.write32(r.addr, value); break;
        }
        break;
    case Resolved::Loc::Ccr: cpu.set_ccr(static_cast<u8>(value)); break;
    case Resolved::Loc::Sr: cpu.set_sr(static_cast<u16>(value)); break;
    case Resolved::Loc::Usp:
        if (cpu.s) cpu.usp = value; else cpu.ssp = value;
        break;
    default: break;
    }
}

// The address an EA denotes, without loading from it (LEA, PEA, JMP, JSR).
u32 effective_address(const Operand& o, M68kState& cpu) {
    switch (o.kind) {
    case EAKind::Indirect: return cpu.a[o.reg];
    case EAKind::Displacement: return cpu.a[o.reg] + static_cast<u32>(o.displacement);
    case EAKind::IndexIndirect:
        return cpu.a[o.reg] + static_cast<u32>(o.displacement) + index_value(o.index, cpu);
    case EAKind::AbsW:
    case EAKind::AbsL:
    case EAKind::PcDisplacement: return o.value;
    case EAKind::PcIndex:
        return o.value + static_cast<u32>(o.displacement) + index_value(o.index, cpu);
    default: return 0;
    }
}

u32 apply_shift(Mnemonic m, Size size, M68kState& cpu, u32 value, unsigned count) {
    switch (size) {
    case Size::Byte:
        switch (m) {
        case Mnemonic::Asl: return sem::asl<8>(cpu, value, count);
        case Mnemonic::Asr: return sem::asr<8>(cpu, value, count);
        case Mnemonic::Lsl: return sem::lsl<8>(cpu, value, count);
        case Mnemonic::Lsr: return sem::lsr<8>(cpu, value, count);
        case Mnemonic::Rol: return sem::rol<8>(cpu, value, count);
        case Mnemonic::Ror: return sem::ror<8>(cpu, value, count);
        case Mnemonic::Roxl: return sem::roxl<8>(cpu, value, count);
        default: return sem::roxr<8>(cpu, value, count);
        }
    case Size::Word:
        switch (m) {
        case Mnemonic::Asl: return sem::asl<16>(cpu, value, count);
        case Mnemonic::Asr: return sem::asr<16>(cpu, value, count);
        case Mnemonic::Lsl: return sem::lsl<16>(cpu, value, count);
        case Mnemonic::Lsr: return sem::lsr<16>(cpu, value, count);
        case Mnemonic::Rol: return sem::rol<16>(cpu, value, count);
        case Mnemonic::Ror: return sem::ror<16>(cpu, value, count);
        case Mnemonic::Roxl: return sem::roxl<16>(cpu, value, count);
        default: return sem::roxr<16>(cpu, value, count);
        }
    default:
        switch (m) {
        case Mnemonic::Asl: return sem::asl<32>(cpu, value, count);
        case Mnemonic::Asr: return sem::asr<32>(cpu, value, count);
        case Mnemonic::Lsl: return sem::lsl<32>(cpu, value, count);
        case Mnemonic::Lsr: return sem::lsr<32>(cpu, value, count);
        case Mnemonic::Rol: return sem::rol<32>(cpu, value, count);
        case Mnemonic::Ror: return sem::ror<32>(cpu, value, count);
        case Mnemonic::Roxl: return sem::roxl<32>(cpu, value, count);
        default: return sem::roxr<32>(cpu, value, count);
        }
    }
}

} // namespace

u32 interpret_one(M68kState& cpu, Runtime& rt) {
    const Decoder decoder(&runtime_fetch, &rt);
    const DecodedInstruction in = decoder.decode(cpu.pc);
    const u32 pc = in.pc;
    const u32 next = in.next_pc();
    rt.set_insn_pc(pc);

    // Undecodable opcodes go to the correct guest vector. Never ignored.
    if (!in.valid()) {
        Vector v = Vector::IllegalInstruction;
        if (in.mnemonic == Mnemonic::LineA) v = Vector::LineA;
        else if (in.mnemonic == Mnemonic::LineF) v = Vector::LineF;
        rt.advance_cpu_cycles(cpu, in.base_cycles);
        return rt.enter_exception(cpu, v, pc);
    }

    // Privileged instructions in user mode fault before doing anything.
    if ((in.flags & INSN_PRIVILEGED) && !cpu.s) {
        rt.advance_cpu_cycles(cpu, in.base_cycles);
        return rt.enter_exception(cpu, Vector::PrivilegeViolation, pc);
    }

    u32 result_pc = next;
    const Size size = in.size;

    switch (in.mnemonic) {
    case Mnemonic::Nop:
        break;

    case Mnemonic::Move:
    case Mnemonic::Movea: {
        const Resolved s = resolve(in.src, size, cpu);
        const u32 value = read_resolved(s, size, cpu, rt);
        if (in.mnemonic == Mnemonic::Movea) {
            cpu.a[in.dst.reg] =
                size == Size::Word ? sign_extend_16(static_cast<u16>(value)) : value;
        } else {
            const Resolved d = resolve(in.dst, size, cpu);
            switch (size) {
            case Size::Byte: sem::logic_flags<8>(cpu, value); break;
            case Size::Word: sem::logic_flags<16>(cpu, value); break;
            default: sem::logic_flags<32>(cpu, value); break;
            }
            write_resolved(d, size, value, cpu, rt);
        }
        break;
    }

    case Mnemonic::Moveq:
        cpu.d[in.dst.reg] = in.src.value;
        sem::logic_flags<32>(cpu, in.src.value);
        break;

    case Mnemonic::Lea:
        cpu.a[in.dst.reg] = effective_address(in.src, cpu);
        break;

    case Mnemonic::Pea:
        rt.push32(cpu, effective_address(in.src, cpu));
        break;

    case Mnemonic::Clr: {
        const Resolved d = resolve(in.dst, size, cpu);
        // A 68000 CLR reads before it writes. That matters for hardware.
        (void)read_resolved(d, size, cpu, rt);
        write_resolved(d, size, 0, cpu, rt);
        sem::clr_flags(cpu);
        break;
    }

    case Mnemonic::Tst: {
        const Resolved s = resolve(in.src, size, cpu);
        const u32 value = read_resolved(s, size, cpu, rt);
        switch (size) {
        case Size::Byte: sem::logic_flags<8>(cpu, value); break;
        case Size::Word: sem::logic_flags<16>(cpu, value); break;
        default: sem::logic_flags<32>(cpu, value); break;
        }
        break;
    }

    case Mnemonic::Add: case Mnemonic::Addi: case Mnemonic::Addq:
    case Mnemonic::Sub: case Mnemonic::Subi: case Mnemonic::Subq:
    case Mnemonic::And: case Mnemonic::Andi:
    case Mnemonic::Or:  case Mnemonic::Ori:
    case Mnemonic::Eor: case Mnemonic::Eori: {
        // ADDQ/SUBQ on an address register are full-width and set no flags.
        if ((in.mnemonic == Mnemonic::Addq || in.mnemonic == Mnemonic::Subq) &&
            in.dst.kind == EAKind::AddrReg) {
            const u32 delta = in.src.value;
            cpu.a[in.dst.reg] += (in.mnemonic == Mnemonic::Addq) ? delta : -delta;
            break;
        }
        const Resolved s = resolve(in.src, size, cpu);
        const u32 src = read_resolved(s, size, cpu, rt);
        const Resolved d = resolve(in.dst, size, cpu);
        const u32 dst = read_resolved(d, size, cpu, rt);
        u32 res = 0;
        switch (in.mnemonic) {
        case Mnemonic::Add: case Mnemonic::Addi: case Mnemonic::Addq:
            res = size == Size::Byte ? sem::add<8>(cpu, dst, src)
                : size == Size::Word ? sem::add<16>(cpu, dst, src)
                                     : sem::add<32>(cpu, dst, src);
            break;
        case Mnemonic::Sub: case Mnemonic::Subi: case Mnemonic::Subq:
            res = size == Size::Byte ? sem::sub<8>(cpu, dst, src)
                : size == Size::Word ? sem::sub<16>(cpu, dst, src)
                                     : sem::sub<32>(cpu, dst, src);
            break;
        case Mnemonic::And: case Mnemonic::Andi: res = dst & src; goto logic;
        case Mnemonic::Or:  case Mnemonic::Ori:  res = dst | src; goto logic;
        default:
            res = dst ^ src;
        logic:
            switch (size) {
            case Size::Byte: sem::logic_flags<8>(cpu, res); break;
            case Size::Word: sem::logic_flags<16>(cpu, res); break;
            default: sem::logic_flags<32>(cpu, res); break;
            }
            break;
        }
        write_resolved(d, size, res, cpu, rt);
        break;
    }

    case Mnemonic::Adda: case Mnemonic::Suba: {
        const Resolved s = resolve(in.src, size, cpu);
        u32 src = read_resolved(s, size, cpu, rt);
        if (size == Size::Word) src = sign_extend_16(static_cast<u16>(src));
        cpu.a[in.dst.reg] += (in.mnemonic == Mnemonic::Adda) ? src : -src;
        break;
    }

    case Mnemonic::Cmp: case Mnemonic::Cmpi: case Mnemonic::Cmpm: {
        const Resolved s = resolve(in.src, size, cpu);
        const u32 src = read_resolved(s, size, cpu, rt);
        const Resolved d = resolve(in.dst, size, cpu);
        const u32 dst = read_resolved(d, size, cpu, rt);
        switch (size) {
        case Size::Byte: sem::cmp<8>(cpu, dst, src); break;
        case Size::Word: sem::cmp<16>(cpu, dst, src); break;
        default: sem::cmp<32>(cpu, dst, src); break;
        }
        break;
    }

    case Mnemonic::Cmpa: {
        const Resolved s = resolve(in.src, size, cpu);
        u32 src = read_resolved(s, size, cpu, rt);
        if (size == Size::Word) src = sign_extend_16(static_cast<u16>(src));
        sem::cmp<32>(cpu, cpu.a[in.dst.reg], src);
        break;
    }

    case Mnemonic::Addx: case Mnemonic::Subx: {
        const Resolved s = resolve(in.src, size, cpu);
        const u32 src = read_resolved(s, size, cpu, rt);
        const Resolved d = resolve(in.dst, size, cpu);
        const u32 dst = read_resolved(d, size, cpu, rt);
        u32 res;
        if (in.mnemonic == Mnemonic::Addx)
            res = size == Size::Byte ? sem::addx<8>(cpu, dst, src)
                : size == Size::Word ? sem::addx<16>(cpu, dst, src)
                                     : sem::addx<32>(cpu, dst, src);
        else
            res = size == Size::Byte ? sem::subx<8>(cpu, dst, src)
                : size == Size::Word ? sem::subx<16>(cpu, dst, src)
                                     : sem::subx<32>(cpu, dst, src);
        write_resolved(d, size, res, cpu, rt);
        break;
    }

    case Mnemonic::Neg: case Mnemonic::Negx: case Mnemonic::Not: {
        const Resolved d = resolve(in.dst, size, cpu);
        const u32 dst = read_resolved(d, size, cpu, rt);
        u32 res;
        if (in.mnemonic == Mnemonic::Neg)
            res = size == Size::Byte ? sem::neg<8>(cpu, dst)
                : size == Size::Word ? sem::neg<16>(cpu, dst)
                                     : sem::neg<32>(cpu, dst);
        else if (in.mnemonic == Mnemonic::Negx)
            res = size == Size::Byte ? sem::negx<8>(cpu, dst)
                : size == Size::Word ? sem::negx<16>(cpu, dst)
                                     : sem::negx<32>(cpu, dst);
        else {
            res = ~dst;
            switch (size) {
            case Size::Byte: sem::logic_flags<8>(cpu, res); break;
            case Size::Word: sem::logic_flags<16>(cpu, res); break;
            default: sem::logic_flags<32>(cpu, res); break;
            }
        }
        write_resolved(d, size, res, cpu, rt);
        break;
    }

    case Mnemonic::Nbcd: {
        const Resolved d = resolve(in.dst, Size::Byte, cpu);
        const u8 dst = static_cast<u8>(read_resolved(d, Size::Byte, cpu, rt));
        write_resolved(d, Size::Byte, sem::nbcd(cpu, dst), cpu, rt);
        break;
    }

    case Mnemonic::Abcd: case Mnemonic::Sbcd: {
        const Resolved s = resolve(in.src, Size::Byte, cpu);
        const u8 src = static_cast<u8>(read_resolved(s, Size::Byte, cpu, rt));
        const Resolved d = resolve(in.dst, Size::Byte, cpu);
        const u8 dst = static_cast<u8>(read_resolved(d, Size::Byte, cpu, rt));
        const u8 res = in.mnemonic == Mnemonic::Abcd ? sem::abcd(cpu, dst, src)
                                                     : sem::sbcd(cpu, dst, src);
        write_resolved(d, Size::Byte, res, cpu, rt);
        break;
    }

    case Mnemonic::Mulu: case Mnemonic::Muls: {
        const Resolved s = resolve(in.src, Size::Word, cpu);
        const u16 src = static_cast<u16>(read_resolved(s, Size::Word, cpu, rt));
        const u16 dst = static_cast<u16>(cpu.d[in.dst.reg]);
        cpu.d[in.dst.reg] = in.mnemonic == Mnemonic::Mulu ? sem::mulu(cpu, dst, src)
                                                          : sem::muls(cpu, dst, src);
        break;
    }

    case Mnemonic::Divu: case Mnemonic::Divs: {
        const Resolved s = resolve(in.src, Size::Word, cpu);
        const u16 src = static_cast<u16>(read_resolved(s, Size::Word, cpu, rt));
        const u32 dst = cpu.d[in.dst.reg];
        const sem::DivResult r = in.mnemonic == Mnemonic::Divu ? sem::divu(cpu, dst, src)
                                                               : sem::divs(cpu, dst, src);
        if (r.divide_by_zero) {
            rt.advance_cpu_cycles(cpu, in.base_cycles);
            return rt.enter_exception(cpu, Vector::DivideByZero, next);
        }
        if (!r.overflow) cpu.d[in.dst.reg] = r.value;
        break;
    }

    case Mnemonic::Ext:
        if (size == Size::Word)
            cpu.d[in.dst.reg] = replace_lo16(
                cpu.d[in.dst.reg],
                static_cast<u16>(sign_extend_8(static_cast<u8>(cpu.d[in.dst.reg]))));
        else
            cpu.d[in.dst.reg] = sign_extend_16(static_cast<u16>(cpu.d[in.dst.reg]));
        if (size == Size::Word) sem::logic_flags<16>(cpu, cpu.d[in.dst.reg]);
        else sem::logic_flags<32>(cpu, cpu.d[in.dst.reg]);
        break;

    case Mnemonic::Swap: {
        const u32 v = cpu.d[in.dst.reg];
        const u32 res = (v >> 16) | (v << 16);
        cpu.d[in.dst.reg] = res;
        sem::logic_flags<32>(cpu, res);
        break;
    }

    case Mnemonic::Exg: {
        u32& a = in.src.kind == EAKind::DataReg ? cpu.d[in.src.reg] : cpu.a[in.src.reg];
        u32& b = in.dst.kind == EAKind::DataReg ? cpu.d[in.dst.reg] : cpu.a[in.dst.reg];
        const u32 t = a;
        a = b;
        b = t;
        break;
    }

    case Mnemonic::Btst: case Mnemonic::Bchg: case Mnemonic::Bclr:
    case Mnemonic::Bset: {
        const Resolved s = resolve(in.src, Size::Byte, cpu);
        const u32 bit_source = read_resolved(s, in.src.kind == EAKind::DataReg
                                                    ? Size::Long : Size::Byte,
                                             cpu, rt);
        const bool to_register = in.dst.kind == EAKind::DataReg;
        const unsigned bit = bit_source & (to_register ? 31u : 7u);
        const Size op_size = to_register ? Size::Long : Size::Byte;
        const Resolved d = resolve(in.dst, op_size, cpu);
        const u32 value = read_resolved(d, op_size, cpu, rt);
        sem::btst_flag(cpu, value, bit);
        if (in.mnemonic != Mnemonic::Btst) {
            u32 res = value;
            if (in.mnemonic == Mnemonic::Bchg) res ^= (1u << bit);
            else if (in.mnemonic == Mnemonic::Bclr) res &= ~(1u << bit);
            else res |= (1u << bit);
            write_resolved(d, op_size, res, cpu, rt);
        }
        break;
    }

    case Mnemonic::Tas: {
        const Resolved d = resolve(in.dst, Size::Byte, cpu);
        const u8 value = static_cast<u8>(read_resolved(d, Size::Byte, cpu, rt));
        sem::logic_flags<8>(cpu, value);
        write_resolved(d, Size::Byte, static_cast<u8>(value | 0x80), cpu, rt);
        break;
    }

    case Mnemonic::Asl: case Mnemonic::Asr: case Mnemonic::Lsl:
    case Mnemonic::Lsr: case Mnemonic::Rol: case Mnemonic::Ror:
    case Mnemonic::Roxl: case Mnemonic::Roxr: {
        if (in.dst.kind == EAKind::DataReg) {
            unsigned count;
            if (in.src.kind == EAKind::DataReg) count = cpu.d[in.src.reg] & 63u;
            else count = in.src.value;
            const u32 value = read_resolved(resolve(in.dst, size, cpu), size, cpu, rt);
            const u32 res = apply_shift(in.mnemonic, size, cpu, value, count);
            write_resolved(resolve(in.dst, size, cpu), size, res, cpu, rt);
        } else {
            // Memory form: one bit, one word.
            const Resolved d = resolve(in.dst, Size::Word, cpu);
            const u32 value = read_resolved(d, Size::Word, cpu, rt);
            const u32 res = apply_shift(in.mnemonic, Size::Word, cpu, value, 1);
            write_resolved(d, Size::Word, res, cpu, rt);
        }
        break;
    }

    case Mnemonic::Scc: {
        const u8 value = test_cond(cpu, in.condition) ? 0xff : 0x00;
        const Resolved d = resolve(in.dst, Size::Byte, cpu);
        write_resolved(d, Size::Byte, value, cpu, rt);
        break;
    }

    case Mnemonic::Dbcc:
        if (!test_cond(cpu, in.condition)) {
            const u16 counter = static_cast<u16>(cpu.d[in.dst.reg] - 1);
            cpu.d[in.dst.reg] = replace_lo16(cpu.d[in.dst.reg], counter);
            if (counter != 0xffff) result_pc = in.target;
        }
        break;

    case Mnemonic::Bra:
        result_pc = in.target;
        break;

    case Mnemonic::Bcc:
        if (test_cond(cpu, in.condition)) result_pc = in.target;
        break;

    case Mnemonic::Bsr:
        rt.push32(cpu, next);
        result_pc = in.target;
        break;

    case Mnemonic::Jmp:
        result_pc = effective_address(in.src, cpu);
        break;

    case Mnemonic::Jsr:
        // The target is computed before the return address is pushed, which
        // matters for jsr (a7) and similar.
        result_pc = effective_address(in.src, cpu);
        rt.push32(cpu, next);
        break;

    case Mnemonic::Rts:
        result_pc = rt.pop32(cpu);
        break;

    case Mnemonic::Rtr: {
        const u16 ccr = rt.pop16(cpu);
        cpu.set_ccr(static_cast<u8>(ccr & SR_CCR_MASK));
        result_pc = rt.pop32(cpu);
        break;
    }

    case Mnemonic::Rte: {
        const u16 sr = rt.pop16(cpu);
        result_pc = rt.pop32(cpu);
        cpu.set_sr(sr);
        break;
    }

    case Mnemonic::Link: {
        const u32 disp = sign_extend_16(static_cast<u16>(in.src.value));
        rt.push32(cpu, cpu.a[in.dst.reg]);
        cpu.a[in.dst.reg] = cpu.a[7];
        cpu.a[7] += disp;
        break;
    }

    case Mnemonic::Unlk:
        cpu.a[7] = cpu.a[in.dst.reg];
        cpu.a[in.dst.reg] = rt.pop32(cpu);
        break;

    case Mnemonic::Movem: {
        const bool to_registers = in.dst.kind == EAKind::RegList;
        const u16 mask = static_cast<u16>(to_registers ? in.dst.value : in.src.value);
        const Operand& ea = to_registers ? in.src : in.dst;
        const u32 step = size_bytes(size);

        if (!to_registers && ea.kind == EAKind::PreDec) {
            // Predecrement stores in reverse order, and the mask is reversed
            // with it: bit 0 is A7, bit 15 is D0.
            u32 addr = cpu.a[ea.reg];
            for (int i = 0; i < 16; ++i) {
                if (!(mask & (1u << i))) continue;
                const int index = 15 - i;
                const u32 value = index < 8 ? cpu.d[index] : cpu.a[index - 8];
                addr -= step;
                if (size == Size::Word) rt.write16(addr, static_cast<u16>(value));
                else rt.write32(addr, value);
            }
            cpu.a[ea.reg] = addr;
            break;
        }

        u32 addr = ea.kind == EAKind::PostInc ? cpu.a[ea.reg] : effective_address(ea, cpu);
        for (int i = 0; i < 16; ++i) {
            if (!(mask & (1u << i))) continue;
            if (to_registers) {
                u32 value;
                if (size == Size::Word) value = sign_extend_16(rt.read16(addr));
                else value = rt.read32(addr);
                if (i < 8) cpu.d[i] = value; else cpu.a[i - 8] = value;
            } else {
                const u32 value = i < 8 ? cpu.d[i] : cpu.a[i - 8];
                if (size == Size::Word) rt.write16(addr, static_cast<u16>(value));
                else rt.write32(addr, value);
            }
            addr += step;
        }
        if (ea.kind == EAKind::PostInc) cpu.a[ea.reg] = addr;
        break;
    }

    case Mnemonic::Movep: {
        const bool to_memory = in.dst.kind == EAKind::Displacement;
        const Operand& mem = to_memory ? in.dst : in.src;
        const u8 dreg = to_memory ? in.src.reg : in.dst.reg;
        u32 addr = cpu.a[mem.reg] + static_cast<u32>(mem.displacement);
        const int count = size == Size::Long ? 4 : 2;
        if (to_memory) {
            for (int i = count - 1; i >= 0; --i) {
                rt.write8(addr, static_cast<u8>(cpu.d[dreg] >> (8 * i)));
                addr += 2;
            }
        } else {
            u32 value = 0;
            for (int i = count - 1; i >= 0; --i) {
                value |= static_cast<u32>(rt.read8(addr)) << (8 * i);
                addr += 2;
            }
            if (size == Size::Long) cpu.d[dreg] = value;
            else cpu.d[dreg] = replace_lo16(cpu.d[dreg], static_cast<u16>(value));
        }
        break;
    }

    case Mnemonic::MoveFromSr: {
        const Resolved d = resolve(in.dst, Size::Word, cpu);
        write_resolved(d, Size::Word, cpu.sr(), cpu, rt);
        break;
    }

    case Mnemonic::MoveToCcr: {
        const Resolved s = resolve(in.src, Size::Word, cpu);
        cpu.set_ccr(static_cast<u8>(read_resolved(s, Size::Word, cpu, rt) & SR_CCR_MASK));
        break;
    }

    case Mnemonic::MoveToSr: {
        const Resolved s = resolve(in.src, Size::Word, cpu);
        cpu.set_sr(static_cast<u16>(read_resolved(s, Size::Word, cpu, rt)));
        break;
    }

    case Mnemonic::MoveAnToUsp:
        cpu.usp = cpu.a[in.src.reg];
        break;

    case Mnemonic::MoveUspToAn:
        cpu.a[in.dst.reg] = cpu.usp;
        break;

    case Mnemonic::OriToCcr:
        cpu.set_ccr(static_cast<u8>((cpu.ccr() | in.src.value) & SR_CCR_MASK));
        break;
    case Mnemonic::AndiToCcr:
        cpu.set_ccr(static_cast<u8>(cpu.ccr() & in.src.value & SR_CCR_MASK));
        break;
    case Mnemonic::EoriToCcr:
        cpu.set_ccr(static_cast<u8>((cpu.ccr() ^ in.src.value) & SR_CCR_MASK));
        break;
    case Mnemonic::OriToSr:
        cpu.set_sr(static_cast<u16>(cpu.sr() | in.src.value));
        break;
    case Mnemonic::AndiToSr:
        cpu.set_sr(static_cast<u16>(cpu.sr() & in.src.value));
        break;
    case Mnemonic::EoriToSr:
        cpu.set_sr(static_cast<u16>(cpu.sr() ^ in.src.value));
        break;

    case Mnemonic::Chk: {
        const Resolved s = resolve(in.src, Size::Word, cpu);
        const s16 bound = static_cast<s16>(read_resolved(s, Size::Word, cpu, rt));
        const s16 value = static_cast<s16>(cpu.d[in.dst.reg]);
        if (value < 0 || value > bound) {
            cpu.n = value < 0;
            rt.advance_cpu_cycles(cpu, in.base_cycles);
            return rt.enter_exception(cpu, Vector::ChkInstruction, next);
        }
        break;
    }

    case Mnemonic::Trap:
        rt.advance_cpu_cycles(cpu, in.base_cycles);
        return rt.enter_exception_index(
            cpu, static_cast<u32>(Vector::TrapBase) + in.src.value, next);

    case Mnemonic::Trapv:
        if (cpu.v) {
            rt.advance_cpu_cycles(cpu, in.base_cycles);
            return rt.enter_exception(cpu, Vector::TrapV, next);
        }
        break;

    case Mnemonic::Stop:
        cpu.set_sr(static_cast<u16>(in.src.value));
        cpu.stopped = true;
        break;

    case Mnemonic::Reset:
        // Asserts the reset line. Peripherals reset; the CPU does not.
        break;

    case Mnemonic::IllegalOp:
        rt.advance_cpu_cycles(cpu, in.base_cycles);
        return rt.enter_exception(cpu, Vector::IllegalInstruction, pc);

    default:
        rt.advance_cpu_cycles(cpu, in.base_cycles);
        return rt.enter_exception(cpu, Vector::IllegalInstruction, pc);
    }

    rt.advance_cpu_cycles(cpu, in.base_cycles);

    // Interrupts are taken at instruction boundaries, exactly as generated
    // code does it.
    if (rt.interrupt_should_preempt(cpu))
        return rt.enter_pending_interrupt(cpu, result_pc);

    return result_pc & rt.address_mask();
}

u32 interpret_block(M68kState& cpu, Runtime& rt) { return interpret_one(cpu, rt); }

} // namespace arecomp
