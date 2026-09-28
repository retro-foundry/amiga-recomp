// The game manifest (AMIGA_RECOMP.md 5).
//
// Game-specific knowledge lives here as metadata: entry state, memory map,
// known code ranges, resolved indirect targets, patches and hooks. It is
// deliberately not reconstructed source, and the generic recompiler carries
// none of it.
#pragma once

#include <string>
#include <vector>

#include "amiga_recomp/types.hpp"

namespace arecomp {

enum class InputMode {
    Flat,      // a raw binary loaded at a fixed guest address
    Snapshot,  // a captured memory image plus machine state (mode A)
    Hunk,      // an AmigaDOS hunk executable (mode B)
    Adf,       // a disk image (mode C, not yet implemented)
};

struct MemoryRegionSpec {
    u32 start = 0;
    u32 size = 0;
    std::string kind = "chip";
    std::string permissions = "rwx";
};

// One chunk the disk loader reads into memory. Read off the bootblock with
// `arecomp bootblock`, then recorded here: the framework cannot infer a
// trackloader, but it can be told what one does (AMIGA_RECOMP.md 4.3).
struct AdfLoadSpec {
    u32 offset = 0;      // byte offset within the disk image
    u32 length = 0;
    u32 address = 0;     // guest address it is loaded at
    bool executable = true;
};

struct CodeRangeSpec {
    u32 start = 0;
    u32 end = 0;   // exclusive
};

struct IndirectTargetSpec {
    u32 site = 0;                 // the guest address of the indirect transfer
    std::vector<u32> targets;     // addresses observed or declared for it
};

struct PatchSpec {
    u32 address = 0;
    std::string replacement;      // currently "nop"
};

struct HookSpec {
    u32 address = 0;
    std::string name;
    std::string mode = "before";  // before | after | replace
};

struct CpuStateSpec {
    u32 entry = 0;
    u16 initial_sr = 0x2700;
    u32 d[8] = {};
    u32 a[8] = {};
    bool has_registers = false;
};

struct RecompilerOptions {
    bool strict_address_errors = true;
    bool development_interpreter_fallback = true;
    bool emit_instruction_comments = true;
    bool emit_cycle_accounting = true;
    // Blocks per generated translation unit. Splitting keeps compile times
    // sane on real games, which produce tens of thousands of blocks.
    u32 blocks_per_file = 256;
};

struct Manifest {
    std::string path;          // the manifest's own location, for resolving paths
    std::string name;
    std::string id = "port";
    std::string machine = "a500-pal";
    std::string sha256;

    InputMode input_mode = InputMode::Flat;
    std::string input_path;
    // A disk image to put in the drive. An ADF port gets this for free; a
    // snapshot port needs it too whenever the game keeps loading from disk,
    // which most do.
    std::string disk_path;
    // Saved custom chip state that goes with a snapshot. Without it the game
    // resumes on a machine whose chips it never configured.
    std::string chipset_state_path;
    u32 load_address = 0;      // for flat images

    std::string video_standard = "pal";

    CpuStateSpec cpu;
    RecompilerOptions options;

    std::vector<AdfLoadSpec> adf_loads;
    std::vector<MemoryRegionSpec> regions;
    std::vector<CodeRangeSpec> code_ranges;
    std::vector<IndirectTargetSpec> indirect_targets;
    std::vector<PatchSpec> patches;
    std::vector<HookSpec> hooks;
    std::vector<u32> extra_entry_points;

    // Optional debug sentinel: a guest write here halts the CPU. Used by
    // synthetic test programs and replay harnesses, never by a real game.
    u32 harness_halt_address = 0;

    // Whether the generated port instantiates the custom chips. True for a
    // real game; the synthetic CPU-only test programs turn it off.
    bool enable_chipset = true;

    // Capture guest memory when execution first reaches an address. This is
    // how a self-decrunching or disk-loading game becomes a snapshot the
    // recompiler can translate (docs/decrunching.md).
    struct CaptureSpec {
        bool enabled = false;
        u32 trigger = 0;
        u32 start = 0;
        u32 size = 0;
        bool halt = true;
        std::string path;
    } capture;

    // Level 1 widescreen (docs/widescreen.md). Off unless the port asks.
    struct WidescreenSpec {
        bool enabled = false;
        u32 extra_left = 0;
        u32 extra_right = 0;
        bool compensate_modulo = true;
    } widescreen;

    // Loads and validates. Returns false with a human-readable reason.
    static bool load(const std::string& path, Manifest& out, std::string& error);

    // Directory containing the manifest, used to resolve relative input paths.
    [[nodiscard]] std::string directory() const;
    [[nodiscard]] std::string resolve(const std::string& relative) const;

    [[nodiscard]] bool in_code_range(u32 address) const;
};

} // namespace arecomp
