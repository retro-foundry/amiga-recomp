// arecomp: the static recompiler command line (AMIGA_RECOMP.md 3, 44).

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "amiga_recomp/decoder.hpp"
#include "analyzer.hpp"
#include "codegen.hpp"
#include "image.hpp"
#include "manifest.hpp"
#include "project.hpp"

namespace {

using namespace arecomp;

constexpr const char* kVersion = "0.1.0";

void print_usage() {
    std::printf(
        "arecomp %s - static recompiler for Amiga 68000 code\n"
        "\n"
        "usage:\n"
        "  arecomp recompile <manifest.toml> -o <dir>   translate into a native project\n"
        "  arecomp analyze   <manifest.toml>            report discovery without emitting\n"
        "  arecomp disasm    <manifest.toml> [options]  disassemble discovered blocks\n"
        "  arecomp version\n"
        "\n"
        "options:\n"
        "  -o, --output <dir>    output directory (default: projects/<id>)\n"
        "      --from <addr>     disasm: first address\n"
        "      --count <n>       disasm: number of instructions\n"
        "\n"
        "Generated ports are written under projects/, which is not part of this\n"
        "repository. No game data is ever embedded in the recompiler or runtime.\n",
        kVersion);
}

u32 parse_address(const char* text) {
    return static_cast<u32>(std::strtoul(text, nullptr, 0));
}

bool load_inputs(const std::string& manifest_path, Manifest& manifest, Image& image) {
    std::string error;
    if (!Manifest::load(manifest_path, manifest, error)) {
        std::fprintf(stderr, "arecomp: %s\n", error.c_str());
        return false;
    }
    if (!Image::load(manifest, image, error)) {
        std::fprintf(stderr, "arecomp: %s\n", error.c_str());
        return false;
    }
    return true;
}

void report_analysis(const AnalysisResult& analysis) {
    std::printf("  blocks                : %zu\n", analysis.blocks.size());
    std::printf("  instructions          : %u\n", analysis.instruction_count);
    std::printf("  code bytes            : %u\n", analysis.byte_coverage());
    std::printf("  call targets          : %zu\n", analysis.call_targets.size());
    std::printf("  unresolved indirects  : %zu\n",
                analysis.unresolved_indirect_sites.size());

    u32 warnings = 0, errors = 0;
    for (const Diagnostic& d : analysis.diagnostics) {
        if (d.severity == Severity::Error) ++errors;
        else if (d.severity == Severity::Warning) ++warnings;
    }
    std::printf("  diagnostics           : %u warning(s), %u error(s)\n", warnings, errors);

    for (const Diagnostic& d : analysis.diagnostics) {
        if (d.severity == Severity::Note) continue;
        std::fprintf(stderr, "  %s $%08x: %s\n",
                     d.severity == Severity::Error ? "error  " : "warning",
                     d.address, d.message.c_str());
    }
}

int command_analyze(const std::string& manifest_path) {
    Manifest manifest;
    Image image;
    if (!load_inputs(manifest_path, manifest, image)) return 1;

    Analyzer analyzer(image, manifest);
    const AnalysisResult analysis = analyzer.run();

    std::printf("%s (%s)\n", manifest.name.c_str(), manifest.id.c_str());
    report_analysis(analysis);

    if (!analysis.unresolved_indirect_sites.empty()) {
        std::printf("\nunresolved indirect transfer sites:\n");
        for (u32 site : analysis.unresolved_indirect_sites)
            std::printf("  $%08x\n", site);
        std::printf(
            "\nAdd these to the manifest as [[indirect_target]] entries once a\n"
            "trace run has observed their targets (AMIGA_RECOMP.md 26.2).\n");
    }
    return analysis.has_errors() ? 1 : 0;
}

int command_disasm(const std::string& manifest_path, u32 from, u32 count) {
    Manifest manifest;
    Image image;
    if (!load_inputs(manifest_path, manifest, image)) return 1;

    Analyzer analyzer(image, manifest);
    const AnalysisResult analysis = analyzer.run();

    u32 printed = 0;
    for (const auto& [start, block] : analysis.blocks) {
        if (from && block.end <= from) continue;
        std::printf("\n%s:\n", block_symbol(start).c_str());
        for (const DecodedInstruction& insn : block.instructions) {
            if (from && insn.pc < from) continue;
            std::printf("  %08x  %-40s ; %u cycles\n", insn.pc,
                        disassemble(insn).c_str(), insn.base_cycles);
            if (count && ++printed >= count) return 0;
        }
    }
    return 0;
}

int command_recompile(const std::string& manifest_path, std::string output_dir) {
    Manifest manifest;
    Image image;
    if (!load_inputs(manifest_path, manifest, image)) return 1;

    if (output_dir.empty()) output_dir = "projects/" + manifest.id;

    Analyzer analyzer(image, manifest);
    const AnalysisResult analysis = analyzer.run();

    std::printf("%s (%s)\n", manifest.name.c_str(), manifest.id.c_str());
    report_analysis(analysis);

    if (analysis.has_errors()) {
        std::fprintf(stderr, "arecomp: discovery failed; nothing was emitted\n");
        return 1;
    }
    if (analysis.blocks.empty()) {
        std::fprintf(stderr,
                     "arecomp: no code discovered from entry $%08x. Check the entry\n"
                     "         address, the load address, and the code ranges.\n",
                     manifest.cpu.entry);
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(output_dir + "/generated", ec);

    CodeGenerator generator(analysis, manifest);
    std::string error;
    if (!generator.emit(output_dir, error)) {
        std::fprintf(stderr, "arecomp: %s\n", error.c_str());
        return 1;
    }

    ProjectOptions options;
    options.output_dir = output_dir;
    // The framework root is the parent of the directory holding this binary's
    // source tree; resolved from the current working directory, which is how
    // the tool is invoked from the repository root.
    options.framework_root = std::filesystem::absolute(".").lexically_normal().string();

    if (!write_project(manifest, analysis, options, error)) {
        std::fprintf(stderr, "arecomp: %s\n", error.c_str());
        return 1;
    }
    if (!write_build_report(manifest, analysis, generator.stats(),
                            output_dir + "/build_report.txt", error)) {
        std::fprintf(stderr, "arecomp: %s\n", error.c_str());
        return 1;
    }

    if (!generator.stats().unsupported.empty()) {
        std::fprintf(stderr,
                     "arecomp: %zu instruction(s) have no translation; see %s\n",
                     generator.stats().unsupported.size(),
                     (output_dir + "/build_report.txt").c_str());
        return 1;
    }

    std::printf("\nwrote %u block(s) in %u file(s) to %s\n", generator.stats().blocks,
                generator.stats().files, output_dir.c_str());
    std::printf("build it with:\n"
                "  cmake -S %s -B %s/build -G Ninja\n"
                "  cmake --build %s/build\n",
                output_dir.c_str(), output_dir.c_str(), output_dir.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const std::string command = argv[1];
    if (command == "version" || command == "--version") {
        std::printf("arecomp %s\n", kVersion);
        return 0;
    }
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    if (argc < 3) {
        std::fprintf(stderr, "arecomp: %s needs a manifest path\n", command.c_str());
        return 1;
    }
    const std::string manifest_path = argv[2];

    std::string output_dir;
    u32 from = 0;
    u32 count = 0;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if ((arg == "-o" || arg == "--output") && has_value) output_dir = argv[++i];
        else if (arg == "--from" && has_value) from = parse_address(argv[++i]);
        else if (arg == "--count" && has_value) count = parse_address(argv[++i]);
        else {
            std::fprintf(stderr, "arecomp: unknown option %s\n", arg.c_str());
            return 1;
        }
    }

    if (command == "recompile") return command_recompile(manifest_path, output_dir);
    if (command == "analyze") return command_analyze(manifest_path);
    if (command == "disasm") return command_disasm(manifest_path, from, count);

    std::fprintf(stderr, "arecomp: unknown command %s\n", command.c_str());
    print_usage();
    return 1;
}
