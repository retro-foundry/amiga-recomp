// Code discovery and control-flow analysis (AMIGA_RECOMP.md 8, 26).
//
// The unit of translation is the basic block, not a recovered function. Blocks
// are found by recursive descent from known entry points, with manifest
// metadata supplying what static analysis cannot know: resolved indirect
// targets, extra entry points, and the ranges that actually hold code.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "amiga_recomp/decoder.hpp"
#include "image.hpp"
#include "manifest.hpp"

namespace arecomp {

struct BasicBlock {
    u32 start = 0;
    u32 end = 0;                  // exclusive
    std::vector<DecodedInstruction> instructions;
    std::vector<u32> successors;  // statically known
    bool ends_indirect = false;   // last instruction's target is dynamic
    bool falls_through = false;   // reached the next leader without a transfer
};

enum class Severity { Note, Warning, Error };

struct Diagnostic {
    Severity severity = Severity::Note;
    u32 address = 0;
    std::string message;
};

struct AnalysisResult {
    std::map<u32, BasicBlock> blocks;
    std::set<u32> leaders;
    std::set<u32> call_targets;
    // Sites whose target could not be resolved statically and for which the
    // manifest supplied nothing. These are what a trace run has to fill in.
    std::vector<u32> unresolved_indirect_sites;
    std::vector<Diagnostic> diagnostics;

    u32 instruction_count = 0;
    u32 illegal_count = 0;

    [[nodiscard]] bool has_errors() const;
    [[nodiscard]] u32 byte_coverage() const;
};

class Analyzer {
public:
    Analyzer(const Image& image, const Manifest& manifest)
        : image_(image), manifest_(manifest) {}

    AnalysisResult run();

private:
    // Decode at an address, applying any manifest instruction patch.
    DecodedInstruction decode_at(u32 address) const;
    void discover(u32 address, AnalysisResult& result, std::set<u32>& visited);
    void add_leader(u32 address, AnalysisResult& result, std::vector<u32>& worklist);

    const Image& image_;
    const Manifest& manifest_;
    std::map<u32, DecodedInstruction> decoded_;
};

} // namespace arecomp
