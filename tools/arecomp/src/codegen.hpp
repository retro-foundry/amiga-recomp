// Translation of decoded 68000 basic blocks into C++ (AMIGA_RECOMP.md 11, 29,
// 43).
//
// The output is a literal semantic translation. It manipulates guest
// registers, guest memory and guest flags by name. It does not recover
// structures, infer types, or name anything after gameplay concepts.
#pragma once

#include <string>
#include <vector>

#include "analyzer.hpp"
#include "manifest.hpp"

namespace arecomp {

struct CodegenStats {
    u32 blocks = 0;
    u32 instructions = 0;
    u32 files = 0;
    std::vector<std::string> unsupported;   // mnemonics with no translation
};

class CodeGenerator {
public:
    CodeGenerator(const AnalysisResult& analysis, const Manifest& manifest)
        : analysis_(analysis), manifest_(manifest) {}

    // Writes the generated sources into <out_dir>/generated.
    bool emit(const std::string& out_dir, std::string& error);

    [[nodiscard]] const CodegenStats& stats() const { return stats_; }

private:
    std::string emit_block(const BasicBlock& block);
    // Appends the translation of one instruction. `next_pc` is the guest
    // address of the following instruction.
    void emit_instruction(std::string& out, const DecodedInstruction& insn);

    const AnalysisResult& analysis_;
    const Manifest& manifest_;
    CodegenStats stats_;
};

// The canonical name for the block starting at a guest address. Machine
// oriented by design (AMIGA_RECOMP.md 11).
std::string block_symbol(u32 address);

} // namespace arecomp
