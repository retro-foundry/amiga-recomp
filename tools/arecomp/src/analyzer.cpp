#include "analyzer.hpp"

#include <algorithm>

namespace arecomp {

bool AnalysisResult::has_errors() const {
    for (const Diagnostic& d : diagnostics)
        if (d.severity == Severity::Error) return true;
    return false;
}

u32 AnalysisResult::byte_coverage() const {
    u32 total = 0;
    for (const auto& [start, block] : blocks) total += block.end - block.start;
    return total;
}

DecodedInstruction Analyzer::decode_at(u32 address) const {
    for (const PatchSpec& patch : manifest_.patches) {
        if (patch.address != address) continue;
        // A manifest patch replaces the instruction before translation, so the
        // generated code contains the patch rather than a runtime test
        // (AMIGA_RECOMP.md 32.2).
        DecodedInstruction nop;
        nop.pc = address;
        nop.opcode = 0x4e71;
        nop.mnemonic = Mnemonic::Nop;
        nop.length = 2;
        nop.base_cycles = 4;
        nop.flags = INSN_VALID;
        return nop;
    }
    const Decoder decoder(&Image::fetch, const_cast<Image*>(&image_));
    return decoder.decode(address);
}

void Analyzer::add_leader(u32 address, AnalysisResult& result,
                          std::vector<u32>& worklist) {
    address &= 0x00ffffffu;
    if (!manifest_.in_code_range(address)) return;
    if (!image_.contains(address)) return;
    if (result.leaders.insert(address).second) worklist.push_back(address);
}

void Analyzer::discover(u32 start, AnalysisResult& result, std::set<u32>& visited) {
    u32 address = start;
    std::vector<u32> worklist;

    for (;;) {
        if (!image_.contains(address)) {
            result.diagnostics.push_back(
                {Severity::Warning, address,
                 "control flow leaves the loaded image; declare the target range "
                 "or add an indirect_target entry"});
            break;
        }
        if (!manifest_.in_code_range(address)) {
            result.diagnostics.push_back(
                {Severity::Warning, address,
                 "control flow leaves the declared code ranges"});
            break;
        }
        if (address & 1) {
            result.diagnostics.push_back(
                {Severity::Error, address, "odd instruction address"});
            break;
        }
        if (!visited.insert(address).second) break;   // already walked from here

        const DecodedInstruction insn = decode_at(address);
        decoded_[address] = insn;
        ++result.instruction_count;

        if (!insn.valid()) {
            ++result.illegal_count;
            result.diagnostics.push_back(
                {Severity::Warning, address,
                 "undecodable opcode $" + [&] {
                     char buf[8];
                     std::snprintf(buf, sizeof buf, "%04x", insn.opcode);
                     return std::string(buf);
                 }() + "; discovery stops here (this is usually data reached by "
                       "mistake, or an unresolved indirect branch)"});
            break;
        }

        // A transfer of control ends the block.
        if (insn.flags & INSN_ENDS_BLOCK) {
            if (insn.flags & INSN_INDIRECT) {
                bool resolved = false;
                for (const IndirectTargetSpec& spec : manifest_.indirect_targets) {
                    if (spec.site != address) continue;
                    resolved = true;
                    for (u32 target : spec.targets) add_leader(target, result, worklist);
                }
                if (!resolved && (insn.flags & (INSN_CALL | INSN_RETURN)) != INSN_RETURN) {
                    // RTS is indirect by nature and needs no declaration; a
                    // computed JMP/JSR does.
                    if (!(insn.flags & INSN_RETURN))
                        result.unresolved_indirect_sites.push_back(address);
                }
            } else if (insn.target != 0 || insn.mnemonic == Mnemonic::Bra) {
                add_leader(insn.target, result, worklist);
                if (insn.flags & INSN_CALL) result.call_targets.insert(insn.target);
            }

            // A call, a conditional branch, or a trap continues at the next
            // instruction, which therefore starts a block of its own.
            const bool continues =
                (insn.flags & (INSN_CALL | INSN_CONDITIONAL)) != 0 ||
                insn.mnemonic == Mnemonic::Trap || insn.mnemonic == Mnemonic::Stop;
            if (continues) add_leader(insn.next_pc(), result, worklist);
            break;
        }

        address = insn.next_pc();
    }

    for (u32 leader : worklist) discover(leader, result, visited);
}

AnalysisResult Analyzer::run() {
    AnalysisResult result;
    std::set<u32> visited;
    std::vector<u32> seeds;

    result.leaders.insert(image_.entry() ? image_.entry() : manifest_.cpu.entry);
    for (u32 address : manifest_.extra_entry_points) result.leaders.insert(address);
    // A hook forces a block boundary so the dispatcher can intercept there
    // (AMIGA_RECOMP.md 8.1, 35).
    for (const HookSpec& hook : manifest_.hooks) result.leaders.insert(hook.address);
    // Declared indirect targets are entry points in their own right.
    for (const IndirectTargetSpec& spec : manifest_.indirect_targets)
        for (u32 target : spec.targets) result.leaders.insert(target);

    seeds.assign(result.leaders.begin(), result.leaders.end());
    for (u32 seed : seeds) discover(seed, result, visited);

    // Build blocks from the leader set. A leader in the middle of a previously
    // walked run splits it, which is how a branch into straight-line code is
    // handled without a separate splitting pass.
    for (u32 leader : result.leaders) {
        if (decoded_.find(leader) == decoded_.end()) continue;

        BasicBlock block;
        block.start = leader;
        u32 address = leader;
        for (;;) {
            auto it = decoded_.find(address);
            if (it == decoded_.end()) break;
            const DecodedInstruction& insn = it->second;
            block.instructions.push_back(insn);
            address = insn.next_pc();

            if (insn.flags & INSN_ENDS_BLOCK) {
                if (insn.flags & INSN_INDIRECT) block.ends_indirect = true;
                if (!(insn.flags & INSN_INDIRECT) && insn.target != 0)
                    block.successors.push_back(insn.target);
                if ((insn.flags & (INSN_CALL | INSN_CONDITIONAL)) != 0)
                    block.successors.push_back(insn.next_pc());
                break;
            }
            if (!insn.valid()) break;
            if (result.leaders.count(address) != 0) {
                // Ran into the next block: fall through to it explicitly.
                block.falls_through = true;
                block.successors.push_back(address);
                break;
            }
        }
        block.end = address;
        if (!block.instructions.empty()) result.blocks[leader] = std::move(block);
    }

    std::sort(result.unresolved_indirect_sites.begin(),
              result.unresolved_indirect_sites.end());
    result.unresolved_indirect_sites.erase(
        std::unique(result.unresolved_indirect_sites.begin(),
                    result.unresolved_indirect_sites.end()),
        result.unresolved_indirect_sites.end());

    return result;
}

} // namespace arecomp
