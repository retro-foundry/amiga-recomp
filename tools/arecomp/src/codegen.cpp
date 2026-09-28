#include "codegen.hpp"

#include "amiga_recomp/exceptions.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace arecomp {

namespace {

std::string hex(u32 value) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%08xu", value);
    return buf;
}

std::string hex16(u32 value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%04xu", value & 0xffff);
    return buf;
}

// The width suffix on the shared semantic helpers.
const char* width(Size size) {
    switch (size) {
    case Size::Byte: return "8";
    case Size::Word: return "16";
    default: return "32";
    }
}

const char* cast_type(Size size) {
    switch (size) {
    case Size::Byte: return "uint8_t";
    case Size::Word: return "uint16_t";
    default: return "uint32_t";
    }
}

const char* read_fn(Size size) {
    switch (size) {
    case Size::Byte: return "rt.read8";
    case Size::Word: return "rt.read16";
    default: return "rt.read32";
    }
}

const char* write_fn(Size size) {
    switch (size) {
    case Size::Byte: return "rt.write8";
    case Size::Word: return "rt.write16";
    default: return "rt.write32";
    }
}

std::string reg_name(const Operand& op) {
    return (op.kind == EAKind::AddrReg ? "cpu.a[" : "cpu.d[") +
           std::to_string(op.reg) + "]";
}

// The 68000 moves A7 by two for byte accesses so the stack stays word aligned.
u32 pointer_step(const Operand& op, Size size) {
    if (size == Size::Byte && op.reg == 7) return 2;
    return size_bytes(size);
}

std::string index_expr(const IndexSpec& index) {
    const std::string reg =
        (index.address ? "cpu.a[" : "cpu.d[") + std::to_string(index.reg) + "]";
    return index.long_size ? reg : ("sign_extend_16((uint16_t)" + reg + ")");
}

// Declares `name` as the guest address this operand denotes, applying the
// register side effects of the mode exactly once.
std::string ea_setup(const Operand& op, Size size, const std::string& name) {
    const std::string a = "cpu.a[" + std::to_string(op.reg) + "]";
    switch (op.kind) {
    case EAKind::Indirect:
        return "const uint32_t " + name + " = " + a + ";";
    case EAKind::PostInc:
        return "const uint32_t " + name + " = " + a + "; " + a + " += " +
               std::to_string(pointer_step(op, size)) + "u;";
    case EAKind::PreDec:
        return a + " -= " + std::to_string(pointer_step(op, size)) +
               "u; const uint32_t " + name + " = " + a + ";";
    case EAKind::Displacement:
        return "const uint32_t " + name + " = " + a + " + " +
               hex(static_cast<u32>(op.displacement)) + ";";
    case EAKind::IndexIndirect:
        return "const uint32_t " + name + " = " + a + " + " +
               hex(static_cast<u32>(op.displacement)) + " + " +
               index_expr(op.index) + ";";
    case EAKind::AbsW:
    case EAKind::AbsL:
    case EAKind::PcDisplacement:
        // Statically known: folded to a constant rather than recomputed.
        return "const uint32_t " + name + " = " + hex(op.value) + ";";
    case EAKind::PcIndex:
        return "const uint32_t " + name + " = " +
               hex(op.value + static_cast<u32>(op.displacement)) + " + " +
               index_expr(op.index) + ";";
    default:
        return {};
    }
}

std::string ea_read(const Operand& op, Size size, const std::string& name) {
    switch (op.kind) {
    case EAKind::DataReg:
        switch (size) {
        case Size::Byte: return "(cpu.d[" + std::to_string(op.reg) + "] & 0xffu)";
        case Size::Word: return "(cpu.d[" + std::to_string(op.reg) + "] & 0xffffu)";
        default: return "cpu.d[" + std::to_string(op.reg) + "]";
        }
    case EAKind::AddrReg:
        return size == Size::Word
                   ? "(cpu.a[" + std::to_string(op.reg) + "] & 0xffffu)"
                   : "cpu.a[" + std::to_string(op.reg) + "]";
    case EAKind::Immediate:
        return hex(op.value);
    case EAKind::Ccr: return "((uint32_t)cpu.ccr())";
    case EAKind::Sr: return "((uint32_t)cpu.sr())";
    case EAKind::Usp: return "cpu.inactive_sp()";
    default:
        return std::string(read_fn(size)) + "(" + name + ")";
    }
}

std::string ea_write(const Operand& op, Size size, const std::string& name,
                     const std::string& value) {
    switch (op.kind) {
    case EAKind::DataReg: {
        const std::string r = "cpu.d[" + std::to_string(op.reg) + "]";
        switch (size) {
        case Size::Byte: return r + " = replace_lo8(" + r + ", (uint8_t)(" + value + "));";
        case Size::Word: return r + " = replace_lo16(" + r + ", (uint16_t)(" + value + "));";
        default: return r + " = (" + value + ");";
        }
    }
    case EAKind::AddrReg: {
        const std::string r = "cpu.a[" + std::to_string(op.reg) + "]";
        // Writes to an address register are full width, sign-extended from a
        // word.
        return size == Size::Word
                   ? r + " = sign_extend_16((uint16_t)(" + value + "));"
                   : r + " = (" + value + ");";
    }
    case EAKind::Ccr: return "cpu.set_ccr((uint8_t)(" + value + "));";
    case EAKind::Sr: return "cpu.set_sr((uint16_t)(" + value + "));";
    case EAKind::Usp:
        return "if (cpu.s) cpu.usp = (" + value + "); else cpu.ssp = (" + value + ");";
    default:
        return std::string(write_fn(size)) + "(" + name + ", (" +
               cast_type(size) + ")(" + value + "));";
    }
}

bool touches_memory(const DecodedInstruction& insn) {
    // LEA computes an address without reading through it.
    if (insn.mnemonic == Mnemonic::Lea) return false;
    return insn.src.is_memory() || insn.dst.is_memory() ||
           insn.mnemonic == Mnemonic::Bsr || insn.mnemonic == Mnemonic::Jsr ||
           insn.mnemonic == Mnemonic::Rts || insn.mnemonic == Mnemonic::Rte ||
           insn.mnemonic == Mnemonic::Rtr || insn.mnemonic == Mnemonic::Pea ||
           insn.mnemonic == Mnemonic::Link || insn.mnemonic == Mnemonic::Unlk ||
           insn.mnemonic == Mnemonic::Movem || insn.mnemonic == Mnemonic::Movep;
}

std::string cond_expr(Cond cc) {
    return std::string("test_cond(cpu, Cond::") + [&] {
        switch (cc) {
        case Cond::T: return "T";
        case Cond::F: return "F";
        case Cond::HI: return "HI";
        case Cond::LS: return "LS";
        case Cond::CC: return "CC";
        case Cond::CS: return "CS";
        case Cond::NE: return "NE";
        case Cond::EQ: return "EQ";
        case Cond::VC: return "VC";
        case Cond::VS: return "VS";
        case Cond::PL: return "PL";
        case Cond::MI: return "MI";
        case Cond::GE: return "GE";
        case Cond::LT: return "LT";
        case Cond::GT: return "GT";
        default: return "LE";
        }
    }() + ")";
}

// The address an operand denotes, as an expression, for LEA/PEA/JMP/JSR.
std::string address_expr(const Operand& op) {
    const std::string a = "cpu.a[" + std::to_string(op.reg) + "]";
    switch (op.kind) {
    case EAKind::Indirect: return a;
    case EAKind::Displacement:
        return "(" + a + " + " + hex(static_cast<u32>(op.displacement)) + ")";
    case EAKind::IndexIndirect:
        return "(" + a + " + " + hex(static_cast<u32>(op.displacement)) + " + " +
               index_expr(op.index) + ")";
    case EAKind::AbsW:
    case EAKind::AbsL:
    case EAKind::PcDisplacement:
        return hex(op.value);
    case EAKind::PcIndex:
        return "(" + hex(op.value + static_cast<u32>(op.displacement)) + " + " +
               index_expr(op.index) + ")";
    default: return "0u";
    }
}

} // namespace

std::string block_symbol(u32 address) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "block_%08x", address);
    return buf;
}

// ---------------------------------------------------------------------------

void CodeGenerator::emit_instruction(std::string& out, const DecodedInstruction& insn) {
    const std::string pc = hex(insn.pc);
    const std::string next = hex(insn.next_pc());
    const std::string cycles = std::to_string(insn.base_cycles);
    const Size size = insn.size;
    const std::string w = width(size);
    const char* indent = "    ";

    if (manifest_.options.emit_instruction_comments) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s// %08x  %s\n", indent, insn.pc,
                      disassemble(insn).c_str());
        out += buf;
    }

    auto line = [&](const std::string& text) { out += indent + text + "\n"; };
    auto advance = [&](const std::string& resume) {
        if (manifest_.options.emit_cycle_accounting)
            line("ARECOMP_ADVANCE(" + cycles + ", " + resume + ");");
    };
    // A guest exception aborts the instruction, so time is accounted directly
    // rather than through ARECOMP_ADVANCE, which would try to return a PC.
    auto raise = [&](const std::string& vector, const std::string& stacked) {
        line("rt.advance_cpu_cycles(cpu, " + cycles + ");");
        line("return rt.enter_exception(cpu, " + vector + ", " + stacked + ");");
    };

    if (touches_memory(insn)) line("ARECOMP_INSN(" + pc + ");");

    // Undecodable opcodes route to the right guest vector. Never skipped.
    if (!insn.valid()) {
        const char* vector = insn.mnemonic == Mnemonic::LineA   ? "Vector::LineA"
                             : insn.mnemonic == Mnemonic::LineF ? "Vector::LineF"
                                                                : "Vector::IllegalInstruction";
        raise(vector, pc);
        return;
    }

    if (insn.flags & INSN_PRIVILEGED) {
        line("if (!cpu.s) {");
        line("    rt.advance_cpu_cycles(cpu, " + cycles + ");");
        line("    return rt.enter_exception(cpu, Vector::PrivilegeViolation, " + pc + ");");
        line("}");
    }

    const std::string src_ea = "ea_src";
    const std::string dst_ea = "ea_dst";
    const std::string src_setup = ea_setup(insn.src, size, src_ea);
    const std::string dst_setup = ea_setup(insn.dst, size, dst_ea);

    switch (insn.mnemonic) {
    case Mnemonic::Nop:
    case Mnemonic::Reset:
        break;

    case Mnemonic::Move: {
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    const " + std::string(cast_type(size)) + " value = (" +
             std::string(cast_type(size)) + ")" + ea_read(insn.src, size, src_ea) + ";");
        if (!dst_setup.empty()) line("    " + dst_setup);
        line("    logic_flags" + w + "(cpu, value);");
        line("    " + ea_write(insn.dst, size, dst_ea, "value"));
        line("}");
        break;
    }

    case Mnemonic::Movea: {
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    " + ea_write(insn.dst, size, dst_ea, ea_read(insn.src, size, src_ea)));
        line("}");
        break;
    }

    case Mnemonic::Moveq:
        line("cpu.d[" + std::to_string(insn.dst.reg) + "] = " + hex(insn.src.value) + ";");
        line("logic_flags32(cpu, cpu.d[" + std::to_string(insn.dst.reg) + "]);");
        break;

    case Mnemonic::Lea:
        line("cpu.a[" + std::to_string(insn.dst.reg) + "] = " +
             address_expr(insn.src) + ";");
        break;

    case Mnemonic::Pea:
        line("rt.push32(cpu, " + address_expr(insn.src) + ");");
        break;

    case Mnemonic::Clr:
        line("{");
        if (!dst_setup.empty()) line("    " + dst_setup);
        // A 68000 CLR reads before writing, which is observable on hardware.
        if (insn.dst.is_memory())
            line("    (void)" + ea_read(insn.dst, size, dst_ea) + ";");
        line("    " + ea_write(insn.dst, size, dst_ea, "0"));
        line("    clr_flags(cpu);");
        line("}");
        break;

    case Mnemonic::Tst:
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    logic_flags" + w + "(cpu, " + ea_read(insn.src, size, src_ea) + ");");
        line("}");
        break;

    case Mnemonic::Add: case Mnemonic::Addi: case Mnemonic::Addq:
    case Mnemonic::Sub: case Mnemonic::Subi: case Mnemonic::Subq:
    case Mnemonic::And: case Mnemonic::Andi:
    case Mnemonic::Or:  case Mnemonic::Ori:
    case Mnemonic::Eor: case Mnemonic::Eori: {
        // ADDQ/SUBQ targeting an address register are full width and set no
        // flags, whatever the encoded size says.
        if ((insn.mnemonic == Mnemonic::Addq || insn.mnemonic == Mnemonic::Subq) &&
            insn.dst.kind == EAKind::AddrReg) {
            const char* op = insn.mnemonic == Mnemonic::Addq ? " += " : " -= ";
            line("cpu.a[" + std::to_string(insn.dst.reg) + "]" + op +
                 hex(insn.src.value) + ";");
            break;
        }
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    const uint32_t src = " + ea_read(insn.src, size, src_ea) + ";");
        if (!dst_setup.empty()) line("    " + dst_setup);
        line("    const uint32_t dst = " + ea_read(insn.dst, size, dst_ea) + ";");
        switch (insn.mnemonic) {
        case Mnemonic::Add: case Mnemonic::Addi: case Mnemonic::Addq:
            line("    const uint32_t res = add" + w + "(cpu, dst, src);");
            break;
        case Mnemonic::Sub: case Mnemonic::Subi: case Mnemonic::Subq:
            line("    const uint32_t res = sub" + w + "(cpu, dst, src);");
            break;
        case Mnemonic::And: case Mnemonic::Andi:
            line("    const uint32_t res = dst & src;");
            line("    logic_flags" + w + "(cpu, res);");
            break;
        case Mnemonic::Or: case Mnemonic::Ori:
            line("    const uint32_t res = dst | src;");
            line("    logic_flags" + w + "(cpu, res);");
            break;
        default:
            line("    const uint32_t res = dst ^ src;");
            line("    logic_flags" + w + "(cpu, res);");
            break;
        }
        line("    " + ea_write(insn.dst, size, dst_ea, "res"));
        line("}");
        break;
    }

    case Mnemonic::Adda: case Mnemonic::Suba: {
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        const std::string value =
            size == Size::Word
                ? "sign_extend_16((uint16_t)" + ea_read(insn.src, size, src_ea) + ")"
                : ea_read(insn.src, size, src_ea);
        line("    const uint32_t src = " + value + ";");
        line("    cpu.a[" + std::to_string(insn.dst.reg) + "] " +
             (insn.mnemonic == Mnemonic::Adda ? "+= src;" : "-= src;"));
        line("}");
        break;
    }

    case Mnemonic::Cmp: case Mnemonic::Cmpi: case Mnemonic::Cmpm:
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    const uint32_t src = " + ea_read(insn.src, size, src_ea) + ";");
        if (!dst_setup.empty()) line("    " + dst_setup);
        line("    const uint32_t dst = " + ea_read(insn.dst, size, dst_ea) + ";");
        line("    cmp" + w + "(cpu, dst, src);");
        line("}");
        break;

    case Mnemonic::Cmpa:
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    const uint32_t src = " +
             (size == Size::Word
                  ? "sign_extend_16((uint16_t)" + ea_read(insn.src, size, src_ea) + ")"
                  : ea_read(insn.src, size, src_ea)) + ";");
        line("    cmp32(cpu, cpu.a[" + std::to_string(insn.dst.reg) + "], src);");
        line("}");
        break;

    case Mnemonic::Addx: case Mnemonic::Subx: {
        const char* fn = insn.mnemonic == Mnemonic::Addx ? "addx" : "subx";
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    const uint32_t src = " + ea_read(insn.src, size, src_ea) + ";");
        if (!dst_setup.empty()) line("    " + dst_setup);
        line("    const uint32_t dst = " + ea_read(insn.dst, size, dst_ea) + ";");
        line("    const uint32_t res = " + std::string(fn) + w + "(cpu, dst, src);");
        line("    " + ea_write(insn.dst, size, dst_ea, "res"));
        line("}");
        break;
    }

    case Mnemonic::Neg: case Mnemonic::Negx: case Mnemonic::Not: {
        line("{");
        if (!dst_setup.empty()) line("    " + dst_setup);
        line("    const uint32_t dst = " + ea_read(insn.dst, size, dst_ea) + ";");
        if (insn.mnemonic == Mnemonic::Neg)
            line("    const uint32_t res = neg" + w + "(cpu, dst);");
        else if (insn.mnemonic == Mnemonic::Negx)
            line("    const uint32_t res = negx" + w + "(cpu, dst);");
        else {
            line("    const uint32_t res = ~dst;");
            line("    logic_flags" + w + "(cpu, res);");
        }
        line("    " + ea_write(insn.dst, size, dst_ea, "res"));
        line("}");
        break;
    }

    case Mnemonic::Nbcd:
        line("{");
        if (!dst_setup.empty()) line("    " + dst_setup);
        line("    const uint8_t dst = (uint8_t)" + ea_read(insn.dst, Size::Byte, dst_ea) + ";");
        line("    " + ea_write(insn.dst, Size::Byte, dst_ea, "nbcd(cpu, dst)"));
        line("}");
        break;

    case Mnemonic::Abcd: case Mnemonic::Sbcd: {
        const char* fn = insn.mnemonic == Mnemonic::Abcd ? "abcd" : "sbcd";
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    const uint8_t src = (uint8_t)" + ea_read(insn.src, Size::Byte, src_ea) + ";");
        if (!dst_setup.empty()) line("    " + dst_setup);
        line("    const uint8_t dst = (uint8_t)" + ea_read(insn.dst, Size::Byte, dst_ea) + ";");
        line("    " + ea_write(insn.dst, Size::Byte, dst_ea,
                               std::string(fn) + "(cpu, dst, src)"));
        line("}");
        break;
    }

    case Mnemonic::Mulu: case Mnemonic::Muls: {
        const char* fn = insn.mnemonic == Mnemonic::Mulu ? "mulu" : "muls";
        const std::string d = "cpu.d[" + std::to_string(insn.dst.reg) + "]";
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    const uint16_t src = (uint16_t)" + ea_read(insn.src, Size::Word, src_ea) + ";");
        line("    " + d + " = " + fn + "(cpu, (uint16_t)" + d + ", src);");
        line("}");
        break;
    }

    case Mnemonic::Divu: case Mnemonic::Divs: {
        const char* fn = insn.mnemonic == Mnemonic::Divu ? "divu" : "divs";
        const std::string d = "cpu.d[" + std::to_string(insn.dst.reg) + "]";
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    const uint16_t src = (uint16_t)" + ea_read(insn.src, Size::Word, src_ea) + ";");
        line("    const DivResult r = " + std::string(fn) + "(cpu, " + d + ", src);");
        line("    if (r.divide_by_zero) {");
        line("        rt.advance_cpu_cycles(cpu, " + cycles + ");");
        line("        return rt.enter_exception(cpu, Vector::DivideByZero, " + next + ");");
        line("    }");
        line("    if (!r.overflow) " + d + " = r.value;");
        line("}");
        break;
    }

    case Mnemonic::Ext: {
        const std::string d = "cpu.d[" + std::to_string(insn.dst.reg) + "]";
        if (size == Size::Word) {
            line(d + " = replace_lo16(" + d + ", (uint16_t)sign_extend_8((uint8_t)" + d + "));");
            line("logic_flags16(cpu, " + d + ");");
        } else {
            line(d + " = sign_extend_16((uint16_t)" + d + ");");
            line("logic_flags32(cpu, " + d + ");");
        }
        break;
    }

    case Mnemonic::Swap: {
        const std::string d = "cpu.d[" + std::to_string(insn.dst.reg) + "]";
        line(d + " = (" + d + " >> 16) | (" + d + " << 16);");
        line("logic_flags32(cpu, " + d + ");");
        break;
    }

    case Mnemonic::Exg: {
        const std::string a = reg_name(insn.src);
        const std::string b = reg_name(insn.dst);
        line("{ const uint32_t t = " + a + "; " + a + " = " + b + "; " + b + " = t; }");
        break;
    }

    case Mnemonic::Btst: case Mnemonic::Bchg: case Mnemonic::Bclr:
    case Mnemonic::Bset: {
        const bool to_register = insn.dst.kind == EAKind::DataReg;
        const Size op_size = to_register ? Size::Long : Size::Byte;
        const std::string mask = to_register ? "31u" : "7u";
        line("{");
        const std::string bit_src =
            insn.src.kind == EAKind::DataReg
                ? "cpu.d[" + std::to_string(insn.src.reg) + "]"
                : hex(insn.src.value);
        line("    const unsigned bit = (unsigned)((" + bit_src + ") & " + mask + ");");
        const std::string setup = ea_setup(insn.dst, op_size, dst_ea);
        if (!setup.empty()) line("    " + setup);
        line("    const uint32_t value = " + ea_read(insn.dst, op_size, dst_ea) + ";");
        line("    btst_flag(cpu, value, bit);");
        if (insn.mnemonic != Mnemonic::Btst) {
            const char* op = insn.mnemonic == Mnemonic::Bchg   ? "value ^ (1u << bit)"
                             : insn.mnemonic == Mnemonic::Bclr ? "value & ~(1u << bit)"
                                                               : "value | (1u << bit)";
            line("    " + ea_write(insn.dst, op_size, dst_ea, op));
        }
        line("}");
        break;
    }

    case Mnemonic::Tas:
        line("{");
        if (!dst_setup.empty()) line("    " + dst_setup);
        line("    const uint8_t value = (uint8_t)" + ea_read(insn.dst, Size::Byte, dst_ea) + ";");
        line("    logic_flags8(cpu, value);");
        line("    " + ea_write(insn.dst, Size::Byte, dst_ea, "value | 0x80u"));
        line("}");
        break;

    case Mnemonic::Asl: case Mnemonic::Asr: case Mnemonic::Lsl:
    case Mnemonic::Lsr: case Mnemonic::Rol: case Mnemonic::Ror:
    case Mnemonic::Roxl: case Mnemonic::Roxr: {
        const char* fn = [&] {
            switch (insn.mnemonic) {
            case Mnemonic::Asl: return "asl";
            case Mnemonic::Asr: return "asr";
            case Mnemonic::Lsl: return "lsl";
            case Mnemonic::Lsr: return "lsr";
            case Mnemonic::Rol: return "rol";
            case Mnemonic::Ror: return "ror";
            case Mnemonic::Roxl: return "roxl";
            default: return "roxr";
            }
        }();
        if (insn.dst.kind == EAKind::DataReg) {
            const std::string count =
                insn.src.kind == EAKind::DataReg
                    ? "(unsigned)(cpu.d[" + std::to_string(insn.src.reg) + "] & 63u)"
                    : std::to_string(insn.src.value) + "u";
            const std::string d = "cpu.d[" + std::to_string(insn.dst.reg) + "]";
            line("{");
            line("    const uint32_t res = " + std::string(fn) + w + "(cpu, " +
                 ea_read(insn.dst, size, dst_ea) + ", " + count + ");");
            line("    " + ea_write(insn.dst, size, dst_ea, "res"));
            line("}");
        } else {
            // Memory form: one bit, one word.
            line("{");
            const std::string setup = ea_setup(insn.dst, Size::Word, dst_ea);
            if (!setup.empty()) line("    " + setup);
            line("    const uint32_t res = " + std::string(fn) + "16(cpu, " +
                 ea_read(insn.dst, Size::Word, dst_ea) + ", 1u);");
            line("    " + ea_write(insn.dst, Size::Word, dst_ea, "res"));
            line("}");
        }
        break;
    }

    case Mnemonic::Scc:
        line("{");
        if (!dst_setup.empty()) line("    " + dst_setup);
        line("    const uint8_t value = " + cond_expr(insn.condition) +
             " ? 0xffu : 0x00u;");
        line("    " + ea_write(insn.dst, Size::Byte, dst_ea, "value"));
        line("}");
        break;

    case Mnemonic::MoveFromSr:
        line("{");
        if (!dst_setup.empty()) line("    " + dst_setup);
        line("    " + ea_write(insn.dst, Size::Word, dst_ea, "cpu.sr()"));
        line("}");
        break;

    case Mnemonic::MoveToCcr:
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    cpu.set_ccr((uint8_t)(" + ea_read(insn.src, Size::Word, src_ea) +
             " & SR_CCR_MASK));");
        line("}");
        break;

    case Mnemonic::MoveToSr:
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    cpu.set_sr((uint16_t)" + ea_read(insn.src, Size::Word, src_ea) + ");");
        line("}");
        break;

    case Mnemonic::MoveAnToUsp:
        line("cpu.usp = cpu.a[" + std::to_string(insn.src.reg) + "];");
        break;
    case Mnemonic::MoveUspToAn:
        line("cpu.a[" + std::to_string(insn.dst.reg) + "] = cpu.usp;");
        break;

    case Mnemonic::OriToCcr:
        line("cpu.set_ccr((uint8_t)((cpu.ccr() | " + hex(insn.src.value) + ") & SR_CCR_MASK));");
        break;
    case Mnemonic::AndiToCcr:
        line("cpu.set_ccr((uint8_t)(cpu.ccr() & " + hex(insn.src.value) + " & SR_CCR_MASK));");
        break;
    case Mnemonic::EoriToCcr:
        line("cpu.set_ccr((uint8_t)((cpu.ccr() ^ " + hex(insn.src.value) + ") & SR_CCR_MASK));");
        break;
    case Mnemonic::OriToSr:
        line("cpu.set_sr((uint16_t)(cpu.sr() | " + hex16(insn.src.value) + "));");
        break;
    case Mnemonic::AndiToSr:
        line("cpu.set_sr((uint16_t)(cpu.sr() & " + hex16(insn.src.value) + "));");
        break;
    case Mnemonic::EoriToSr:
        line("cpu.set_sr((uint16_t)(cpu.sr() ^ " + hex16(insn.src.value) + "));");
        break;

    case Mnemonic::Link: {
        const std::string a = "cpu.a[" + std::to_string(insn.dst.reg) + "]";
        line("rt.push32(cpu, " + a + ");");
        line(a + " = cpu.a[7];");
        line("cpu.a[7] += " + hex(sign_extend_16(static_cast<u16>(insn.src.value))) + ";");
        break;
    }

    case Mnemonic::Unlk: {
        const std::string a = "cpu.a[" + std::to_string(insn.dst.reg) + "]";
        line("cpu.a[7] = " + a + ";");
        line(a + " = rt.pop32(cpu);");
        break;
    }

    case Mnemonic::Movem: {
        // The register mask is known at translation time, so this becomes a
        // straight-line sequence rather than a loop over a runtime mask.
        const bool to_registers = insn.dst.kind == EAKind::RegList;
        const u16 mask = static_cast<u16>(to_registers ? insn.dst.value : insn.src.value);
        const Operand& ea = to_registers ? insn.src : insn.dst;
        const u32 step = size_bytes(size);
        const char* store = size == Size::Word ? "rt.write16" : "rt.write32";
        const char* load = size == Size::Word ? "rt.read16" : "rt.read32";

        line("{");
        if (!to_registers && ea.kind == EAKind::PreDec) {
            const std::string a = "cpu.a[" + std::to_string(ea.reg) + "]";
            line("    uint32_t ea = " + a + ";");
            for (int i = 0; i < 16; ++i) {
                if (!(mask & (1u << i))) continue;
                const int index = 15 - i;   // predecrement reverses the mask
                const std::string reg = index < 8
                                            ? "cpu.d[" + std::to_string(index) + "]"
                                            : "cpu.a[" + std::to_string(index - 8) + "]";
                line("    ea -= " + std::to_string(step) + "u;");
                line("    " + std::string(store) + "(ea, (" +
                     std::string(size == Size::Word ? "uint16_t" : "uint32_t") + ")" +
                     reg + ");");
            }
            line("    " + a + " = ea;");
        } else {
            const std::string base = ea.kind == EAKind::PostInc
                                         ? "cpu.a[" + std::to_string(ea.reg) + "]"
                                         : address_expr(ea);
            line("    uint32_t ea = " + base + ";");
            for (int i = 0; i < 16; ++i) {
                if (!(mask & (1u << i))) continue;
                const std::string reg = i < 8 ? "cpu.d[" + std::to_string(i) + "]"
                                              : "cpu.a[" + std::to_string(i - 8) + "]";
                if (to_registers) {
                    // MOVEM.W to a register sign-extends to the full 32 bits.
                    line("    " + reg + " = " +
                         (size == Size::Word ? "sign_extend_16(rt.read16(ea));"
                                             : std::string(load) + "(ea);"));
                } else {
                    line("    " + std::string(store) + "(ea, (" +
                         std::string(size == Size::Word ? "uint16_t" : "uint32_t") + ")" +
                         reg + ");");
                }
                line("    ea += " + std::to_string(step) + "u;");
            }
            if (ea.kind == EAKind::PostInc)
                line("    cpu.a[" + std::to_string(ea.reg) + "] = ea;");
        }
        line("}");
        break;
    }

    case Mnemonic::Movep: {
        const bool to_memory = insn.dst.kind == EAKind::Displacement;
        const Operand& mem = to_memory ? insn.dst : insn.src;
        const u8 dreg = to_memory ? insn.src.reg : insn.dst.reg;
        const int count = size == Size::Long ? 4 : 2;
        const std::string d = "cpu.d[" + std::to_string(dreg) + "]";
        line("{");
        line("    uint32_t ea = cpu.a[" + std::to_string(mem.reg) + "] + " +
             hex(static_cast<u32>(mem.displacement)) + ";");
        if (to_memory) {
            for (int i = count - 1; i >= 0; --i) {
                line("    rt.write8(ea, (uint8_t)(" + d + " >> " +
                     std::to_string(8 * i) + "));");
                line("    ea += 2u;");
            }
        } else {
            line("    uint32_t value = 0;");
            for (int i = count - 1; i >= 0; --i) {
                line("    value |= (uint32_t)rt.read8(ea) << " + std::to_string(8 * i) + ";");
                line("    ea += 2u;");
            }
            line("    " + std::string(size == Size::Long
                                          ? d + " = value;"
                                          : d + " = replace_lo16(" + d + ", (uint16_t)value);"));
        }
        line("}");
        break;
    }

    case Mnemonic::Chk: {
        line("{");
        if (!src_setup.empty()) line("    " + src_setup);
        line("    const int16_t bound = (int16_t)" + ea_read(insn.src, Size::Word, src_ea) + ";");
        line("    const int16_t value = (int16_t)cpu.d[" + std::to_string(insn.dst.reg) + "];");
        line("    if (value < 0 || value > bound) {");
        line("        cpu.n = value < 0;");
        line("        rt.advance_cpu_cycles(cpu, " + cycles + ");");
        line("        return rt.enter_exception(cpu, Vector::ChkInstruction, " + next + ");");
        line("    }");
        line("}");
        break;
    }

    case Mnemonic::Trapv:
        line("if (cpu.v) {");
        line("    rt.advance_cpu_cycles(cpu, " + cycles + ");");
        line("    return rt.enter_exception(cpu, Vector::TrapV, " + next + ");");
        line("}");
        break;

    // --- control transfers: these end the block ---------------------------

    case Mnemonic::Bra:
        advance(hex(insn.target));
        line("return " + hex(insn.target) + ";");
        return;

    case Mnemonic::Bcc:
        line("{");
        line("    const uint32_t next = " + cond_expr(insn.condition) + " ? " +
             hex(insn.target) + " : " + next + ";");
        if (manifest_.options.emit_cycle_accounting)
            line("    ARECOMP_ADVANCE(" + cycles + ", next);");
        line("    return next;");
        line("}");
        return;

    case Mnemonic::Bsr:
        line("rt.push32(cpu, " + next + ");");
        advance(hex(insn.target));
        line("return " + hex(insn.target) + ";");
        return;

    case Mnemonic::Dbcc: {
        const std::string d = "cpu.d[" + std::to_string(insn.dst.reg) + "]";
        line("{");
        line("    uint32_t next = " + next + ";");
        line("    if (!" + cond_expr(insn.condition) + ") {");
        line("        const uint16_t counter = (uint16_t)(" + d + " - 1u);");
        line("        " + d + " = replace_lo16(" + d + ", counter);");
        line("        if (counter != 0xffffu) next = " + hex(insn.target) + ";");
        line("    }");
        if (manifest_.options.emit_cycle_accounting)
            line("    ARECOMP_ADVANCE(" + cycles + ", next);");
        line("    return next;");
        line("}");
        return;
    }

    case Mnemonic::Jmp:
        if (insn.flags & INSN_INDIRECT) {
            line("{");
            line("    const uint32_t next = " + address_expr(insn.src) + " & rt.address_mask();");
            if (manifest_.options.emit_cycle_accounting)
                line("    ARECOMP_ADVANCE(" + cycles + ", next);");
            line("    return next;");
            line("}");
        } else {
            advance(hex(insn.target));
            line("return " + hex(insn.target) + ";");
        }
        return;

    case Mnemonic::Jsr:
        line("{");
        // The target is computed before the return address is pushed, which
        // matters for jsr (a7) and similar.
        line("    const uint32_t target = " + address_expr(insn.src) + " & rt.address_mask();");
        line("    rt.push32(cpu, " + next + ");");
        if (manifest_.options.emit_cycle_accounting)
            line("    ARECOMP_ADVANCE(" + cycles + ", target);");
        line("    return target;");
        line("}");
        return;

    case Mnemonic::Rts:
        line("{");
        line("    const uint32_t next = rt.pop32(cpu) & rt.address_mask();");
        if (manifest_.options.emit_cycle_accounting)
            line("    ARECOMP_ADVANCE(" + cycles + ", next);");
        line("    return next;");
        line("}");
        return;

    case Mnemonic::Rtr:
        line("{");
        line("    cpu.set_ccr((uint8_t)(rt.pop16(cpu) & SR_CCR_MASK));");
        line("    const uint32_t next = rt.pop32(cpu) & rt.address_mask();");
        if (manifest_.options.emit_cycle_accounting)
            line("    ARECOMP_ADVANCE(" + cycles + ", next);");
        line("    return next;");
        line("}");
        return;

    case Mnemonic::Rte:
        line("{");
        line("    const uint16_t sr = rt.pop16(cpu);");
        line("    const uint32_t next = rt.pop32(cpu) & rt.address_mask();");
        line("    cpu.set_sr(sr);");
        if (manifest_.options.emit_cycle_accounting)
            line("    ARECOMP_ADVANCE(" + cycles + ", next);");
        line("    return next;");
        line("}");
        return;

    case Mnemonic::Trap:
        line("rt.advance_cpu_cycles(cpu, " + cycles + ");");
        line("return rt.enter_exception_index(cpu, " +
             std::to_string(static_cast<u32>(Vector::TrapBase) + insn.src.value) +
             "u, " + next + ");");
        return;

    case Mnemonic::Stop:
        line("cpu.set_sr(" + hex16(insn.src.value) + ");");
        line("cpu.stopped = true;");
        advance(next);
        line("return " + next + ";");
        return;

    case Mnemonic::IllegalOp:
        raise("Vector::IllegalInstruction", pc);
        return;

    default:
        // Nothing reaches here while the translator covers the full 68000 user
        // instruction set, but an unsupported instruction must be a build
        // failure, never silently dropped code (rule 9).
        stats_.unsupported.push_back(mnemonic_name(insn.mnemonic));
        line("#error \"arecomp: no translation for " +
             std::string(mnemonic_name(insn.mnemonic)) + "\"");
        return;
    }

    advance(next);
}

std::string CodeGenerator::emit_block(const BasicBlock& block) {
    std::string out;
    char header[160];
    std::snprintf(header, sizeof header,
                  "// guest $%08x - $%08x, %zu instruction(s)\n"
                  "uint32_t %s(M68kState& cpu, Runtime& rt) {\n",
                  block.start, block.end, block.instructions.size(),
                  block_symbol(block.start).c_str());
    out += header;

    for (const DecodedInstruction& insn : block.instructions) {
        emit_instruction(out, insn);
        ++stats_.instructions;
    }

    // A block that ran off its end without a transfer continues at the next
    // guest address, which the dispatcher resolves.
    const DecodedInstruction& last = block.instructions.back();
    if (!(last.flags & INSN_ENDS_BLOCK) || !last.valid()) {
        if (last.valid()) out += "    return " + hex(block.end) + ";\n";
    }
    out += "}\n\n";
    return out;
}

bool CodeGenerator::emit(const std::string& out_dir, std::string& error) {
    const std::string dir = out_dir + "/generated";

    std::vector<u32> addresses;
    addresses.reserve(analysis_.blocks.size());
    for (const auto& [address, block] : analysis_.blocks) addresses.push_back(address);

    const u32 per_file = manifest_.options.blocks_per_file;
    const std::size_t file_count =
        addresses.empty() ? 1 : (addresses.size() + per_file - 1) / per_file;

    // Declarations, so the table can reference every block.
    {
        std::ofstream header(dir + "/blocks.hpp");
        if (!header) {
            error = "cannot write " + dir + "/blocks.hpp";
            return false;
        }
        header << "// Generated by arecomp. Do not edit.\n"
               << "#pragma once\n\n"
               << "#include \"amiga_recomp/recomp_prelude.hpp\"\n\n"
               << "namespace arecomp::generated {\n\n";
        for (u32 address : addresses)
            header << "uint32_t " << block_symbol(address)
                   << "(M68kState& cpu, Runtime& rt);\n";
        header << "\n// Installs every translated block into the dispatcher.\n"
               << "void install_blocks(BlockTable& table);\n\n"
               << "// Guest entry point recorded in the manifest.\n"
               << "constexpr uint32_t kEntryPoint = " << hex(manifest_.cpu.entry) << ";\n"
               << "\n} // namespace arecomp::generated\n";
    }

    for (std::size_t file = 0; file < file_count; ++file) {
        char name[64];
        std::snprintf(name, sizeof name, "/blocks_%03zu.cpp", file);
        std::ofstream source(dir + name);
        if (!source) {
            error = "cannot write " + dir + name;
            return false;
        }
        source << "// Generated by arecomp from " << manifest_.name
               << ". Do not edit.\n"
               << "//\n"
               << "// A literal translation of MC68000 machine code. Guest\n"
               << "// registers, guest addresses and guest flags are named\n"
               << "// directly; nothing here is recovered source.\n\n"
               << "#include \"blocks.hpp\"\n\n"
               << "namespace arecomp::generated {\n\n";

        const std::size_t begin = file * per_file;
        const std::size_t end = std::min(addresses.size(), begin + per_file);
        for (std::size_t i = begin; i < end; ++i) {
            source << emit_block(analysis_.blocks.at(addresses[i]));
            ++stats_.blocks;
        }
        source << "} // namespace arecomp::generated\n";
        ++stats_.files;
    }

    {
        std::ofstream table(dir + "/block_table.cpp");
        if (!table) {
            error = "cannot write " + dir + "/block_table.cpp";
            return false;
        }
        table << "// Generated by arecomp. Do not edit.\n\n"
              << "#include \"blocks.hpp\"\n\n"
              << "namespace arecomp::generated {\n\n"
              << "namespace {\n"
              << "const BlockEntry kBlocks[] = {\n";
        for (u32 address : addresses) {
            // The byte range this block was translated from, so the runtime can
            // retire exactly the blocks a guest write invalidates rather than
            // everything sharing a page with them.
            const BasicBlock& block = analysis_.blocks.at(address);
            table << "    {" << hex(address) << ", &" << block_symbol(address)
                  << ", " << (block.end - block.start) << "},\n";
        }
        if (addresses.empty()) table << "    {0u, nullptr, 0u},\n";
        table << "};\n"
              << "} // namespace\n\n"
              << "void install_blocks(BlockTable& table) {\n"
              << "    table.build(kBlocks, " << addresses.size() << ");\n"
              << "}\n\n"
              << "} // namespace arecomp::generated\n";
    }

    return true;
}

} // namespace arecomp
