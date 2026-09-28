// Generated-port scaffolding (AMIGA_RECOMP.md 44).
//
// A port is an ordinary CMake project that links the runtime and the generated
// blocks. Scaffolding files are written only when absent, so regenerating the
// translation never discards hand-written port code such as hooks.
#pragma once

#include <string>

#include "analyzer.hpp"
#include "codegen.hpp"
#include "manifest.hpp"

namespace arecomp {

struct ProjectOptions {
    std::string output_dir;
    std::string framework_root;   // absolute path to this repository
};

bool write_project(const Manifest& manifest, const AnalysisResult& analysis,
                   const ProjectOptions& options, std::string& error);

// The build report: what was translated, what was not, and what still needs
// resolving before the interpreter fallback can be switched off.
bool write_build_report(const Manifest& manifest, const AnalysisResult& analysis,
                        const CodegenStats& stats, const std::string& path,
                        std::string& error);

} // namespace arecomp
