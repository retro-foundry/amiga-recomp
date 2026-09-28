// Differential testing: the same guest program under the reference
// interpreter and under statically generated native code must produce
// identical guest state (AMIGA_RECOMP.md 28, 64.1).
//
// The generated sources compiled into this binary are produced at build time
// by running arecomp on ports/vertical-slice.toml, so the comparison always
// covers the current translator rather than a checked-in snapshot of it.

#include <cstring>
#include <string>
#include <vector>

#include "amiga_recomp/halt_device.hpp"
#include "amiga_recomp/interpreter.hpp"
#include "amiga_recomp/runtime.hpp"
#include "blocks.hpp"
#include "machine_fixture.hpp"
#include "test_support.hpp"

using namespace arecomp;
using namespace arecomp::test;

namespace {

// Everything the two engines must agree on.
struct FinalState {
    u32 d[8]{};
    u32 a[8]{};
    u32 pc = 0;
    u16 sr = 0;
    u64 cycles = 0;
    bool halted = false;
    bool stopped = false;
    std::vector<u8> work;     // the scratch area the program modifies
    u64 fallbacks = 0;
    u64 blocks = 0;
    u64 steps = 0;
};

enum class Engine { Interpreter, Native };

FinalState run_program(Engine engine, u64 max_steps = 100000) {
    M68kState cpu;
    GuestMemory memory(0x00ffffff);
    memory.add_ram(0x000000, 0x080000, RegionKind::Chip, PERM_RWX, "chip");

    HaltDevice halt(cpu);
    memory.map_device(kHaltAddress, GuestMemory::kPageSize, &halt,
                      RegionKind::Expansion, "halt-sentinel");

    // Load exactly the image the manifest names, at the manifest's address.
    std::FILE* file = std::fopen(testrom_path(ARECOMP_DIFF_IMAGE).c_str(), "rb");
    if (file) {
        std::vector<u8> data;
        u8 buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0)
            data.insert(data.end(), buffer, buffer + n);
        std::fclose(file);
        memory.load(0x1000, data.data(), data.size());
    }

    RuntimeConfig config;
    config.log_unknown_targets = false;
    // The native run must not fall back. If it reaches an address with no
    // compiled block the test should fail loudly, not quietly interpret.
    config.unknown_target = engine == Engine::Native ? UnknownTargetPolicy::Fatal
                                                     : UnknownTargetPolicy::Interpret;

    Runtime runtime(memory, config);
    if (engine == Engine::Interpreter) {
        runtime.set_interpreter(&interpret_block);
    } else {
        generated::install_blocks(runtime.blocks());
    }

    cpu.pc = 0x1000;
    cpu.set_sr(0x2700);
    cpu.a[7] = kStackTop;

    // Both engines run until the guest executes STOP. The halt sentinel sets
    // cpu.halted, but the dispatcher only tests that between blocks, so
    // stopping on it would leave the interpreter (one instruction per step)
    // and the native code (one block per step) at different instructions.
    // STOP ends a block in both, which makes the comparison exact.
    u64 steps = 0;
    while (!cpu.stopped && steps < max_steps) {
        try {
            runtime.step_block(cpu);
        } catch (const GuestFault& fault) {
            cpu.pc = runtime.enter_group0_fault(cpu, fault, cpu.pc);
        } catch (const FatalError&) {
            break;
        }
        ++steps;
    }

    FinalState state;
    std::memcpy(state.d, cpu.d, sizeof state.d);
    std::memcpy(state.a, cpu.a, sizeof state.a);
    state.pc = cpu.pc;
    state.sr = cpu.sr();
    state.cycles = cpu.cycles;
    state.halted = cpu.halted;
    state.stopped = cpu.stopped;
    state.fallbacks = runtime.stats().interpreter_fallbacks;
    state.blocks = runtime.stats().blocks_executed;
    state.steps = steps;
    // The whole scratch area, so a wrong result anywhere in the program shows
    // up as a byte difference rather than passing unnoticed.
    state.work.resize(ARECOMP_DIFF_WORK_SIZE);
    for (std::size_t i = 0; i < state.work.size(); ++i)
        state.work[i] = memory.peek8(
            static_cast<u32>(ARECOMP_DIFF_WORK_BASE + i));
    return state;
}

} // namespace

TEST_CASE("generated native code reproduces the interpreter exactly") {
    const FinalState interpreted = run_program(Engine::Interpreter);
    const FinalState native = run_program(Engine::Native);

    CHECK(interpreted.halted);
    CHECK(native.halted);

    for (int i = 0; i < 8; ++i) {
        if (interpreted.d[i] != native.d[i])
            report_failure(__FILE__, __LINE__,
                           "d" + std::to_string(i) + ": interpreter " +
                               hex(interpreted.d[i]) + ", native " + hex(native.d[i]));
        if (interpreted.a[i] != native.a[i])
            report_failure(__FILE__, __LINE__,
                           "a" + std::to_string(i) + ": interpreter " +
                               hex(interpreted.a[i]) + ", native " + hex(native.a[i]));
    }

    CHECK_EQ(native.pc, interpreted.pc);
    CHECK_EQ(native.sr, interpreted.sr);
    CHECK_EQ(native.stopped, interpreted.stopped);

    // Guest memory must match byte for byte.
    for (std::size_t i = 0; i < interpreted.work.size(); ++i) {
        if (interpreted.work[i] != native.work[i])
            report_failure(__FILE__, __LINE__,
                           "memory $" + hex(ARECOMP_DIFF_WORK_BASE + static_cast<u32>(i)) +
                               ": interpreter " + hex(interpreted.work[i]) +
                               ", native " + hex(native.work[i]));
    }
}

TEST_CASE("guest cycle accounting matches between the two engines") {
    // Both engines charge the same nominal instruction cycles, so a divergence
    // here means the translator dropped or duplicated an ADVANCE.
    const FinalState interpreted = run_program(Engine::Interpreter);
    const FinalState native = run_program(Engine::Native);
    CHECK_EQ(native.cycles, interpreted.cycles);
}

TEST_CASE("the native run uses no interpreter fallback at all") {
    // AMIGA_RECOMP.md 64.9: a finished port completes its replay with zero
    // fallback hits. This is that check, in miniature.
    const FinalState native = run_program(Engine::Native);
    CHECK_EQ(native.fallbacks, 0u);
    CHECK(native.blocks > 0);
}

TEST_CASE("the native run executes whole blocks, not single instructions") {
    // If the block structure had collapsed to one instruction per block the
    // translation would still be correct, but it would have lost its point.
    const FinalState native = run_program(Engine::Native);
    const FinalState interpreted = run_program(Engine::Interpreter);
    CHECK(native.steps < interpreted.steps);
    CHECK_EQ(native.blocks, native.steps);   // one compiled block per step
}

ARECOMP_TEST_MAIN()
