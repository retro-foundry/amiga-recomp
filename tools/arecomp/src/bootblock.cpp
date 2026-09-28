#include "bootblock.hpp"

#include <cstdio>

#include "amiga_recomp/decoder.hpp"
#include "amiga_recomp/guest_memory.hpp"

namespace arecomp {

namespace {

constexpr std::size_t kBootblockSize = 1024;
// The boot block's own code starts after the signature and checksum.
constexpr u32 kCodeStart = 12;

// Offsets into an exec IOStdReq. These are what the boot code fills in.
enum : s32 {
    IO_COMMAND = 0x1c,
    IO_LENGTH = 0x24,
    IO_DATA = 0x28,
    IO_OFFSET = 0x2c,
};

// exec.library DoIO() is at -456, which is -$1c8.
constexpr s32 kDoIoOffset = -0x1c8;

} // namespace

bool analyse_bootblock(const std::vector<u8>& disk, BootblockReport& out,
                       std::string& error) {
    if (disk.size() < kBootblockSize) {
        error = "disk image is smaller than a boot block";
        return false;
    }

    out.signature.assign(reinterpret_cast<const char*>(disk.data()), 4);
    for (char& c : out.signature)
        if (c < 32 || c > 126) c = '.';
    out.has_dos_signature = disk[0] == 'D' && disk[1] == 'O' && disk[2] == 'S';

    // The boot block checksum is a one's-complement sum over its 256 longs.
    u32 sum = 0;
    for (std::size_t i = 0; i < kBootblockSize; i += 4) {
        const u32 value = be32(disk.data() + i);
        const u32 previous = sum;
        sum += value;
        if (sum < previous) ++sum;   // end-around carry
    }
    out.checksum_valid = sum == 0xffffffffu;

    // Walk the code, tracking what is written into the IO request. This is a
    // straight-line read of the common idiom, not an emulation: a boot block
    // that computes its offsets at run time will not be recognised, and the
    // report says so rather than inventing values.
    bool have_length = false, have_data = false, have_offset = false;
    u32 length = 0, data = 0, offset = 0;

    u32 pc = kCodeStart;
    const u32 limit = static_cast<u32>(kBootblockSize);
    int instructions = 0;

    while (pc < limit && instructions < 512) {
        const DecodedInstruction insn =
            decode_buffer(disk.data(), kBootblockSize, 0, pc);
        ++instructions;

        char line[128];
        std::snprintf(line, sizeof line, "%04x  %-40s", pc,
                      disassemble(insn).c_str());
        out.disassembly.emplace_back(line);

        if (!insn.valid()) {
            out.note = "stopped at an undecodable opcode; the boot block may be "
                       "packed or may contain data here";
            break;
        }

        // move.l #imm,d16(a1): filling in the IO request.
        if (insn.mnemonic == Mnemonic::Move && insn.size == Size::Long &&
            insn.src.kind == EAKind::Immediate &&
            insn.dst.kind == EAKind::Displacement) {
            switch (insn.dst.displacement) {
            case IO_LENGTH: length = insn.src.value; have_length = true; break;
            case IO_DATA: data = insn.src.value; have_data = true; break;
            case IO_OFFSET: offset = insn.src.value; have_offset = true; break;
            default: break;
            }
        }

        // jsr d16(a6): a library call. DoIO issues the request.
        if (insn.mnemonic == Mnemonic::Jsr && insn.src.kind == EAKind::Displacement &&
            insn.src.reg == 6 && insn.src.displacement == kDoIoOffset) {
            if (have_length && have_data && have_offset) {
                out.loads.push_back({offset, length, data, pc});
                have_length = have_data = have_offset = false;
            }
        }

        if (insn.mnemonic == Mnemonic::Jmp && !(insn.flags & INSN_INDIRECT)) {
            out.found_entry_jump = true;
            out.entry = insn.target;
            break;
        }

        if (insn.flags & INSN_RETURN) break;

        pc = insn.next_pc();
    }

    if (out.loads.empty() && out.note.empty()) {
        out.note = "no trackdisk read requests recognised; this boot block does "
                   "something the analyser does not know about";
    }
    return true;
}

} // namespace arecomp
