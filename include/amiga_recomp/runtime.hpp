// The native execution environment the generated blocks run inside
// (AMIGA_RECOMP.md 2, 15, 16, 17, 30).
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "amiga_recomp/cpu_state.hpp"
#include "amiga_recomp/dispatch.hpp"
#include "amiga_recomp/exceptions.hpp"
#include "amiga_recomp/guest_memory.hpp"
#include "amiga_recomp/hooks.hpp"
#include "amiga_recomp/types.hpp"

namespace arecomp {

// One master time base for all asynchronous hardware. The unit is a 68000
// clock cycle: 7.09379 MHz on PAL, exactly two colour clocks, exactly ten CIA
// ticks' worth of division (AMIGA.md, display timing). Host wall-clock time
// never influences guest behaviour (AMIGA_RECOMP.md 17, 47).
constexpr u64 kCpuClockPal = 7093790;
constexpr u64 kCpuClockNtsc = 7159090;
constexpr u32 kCpuCyclesPerColourClock = 2;
constexpr u32 kCpuCyclesPerCiaTick = 10;

enum class TimingMode : u8 {
    Instruction,  // nominal instruction cycles only
    BusAware,     // plus memory access cost
    ChipExact,    // plus Agnus stealing Chip RAM slots from the CPU
};

// Everything asynchronous -- Copper, blitter, beam, Paula, CIA -- lives behind
// this. The pure-CPU tests run with none attached.
class HardwareModel {
public:
    virtual ~HardwareModel() = default;
    // Advance hardware up to the given master time. Never past it.
    virtual void run_until(MasterTick target) = 0;
    // Highest interrupt level currently asserted, 0 for none.
    [[nodiscard]] virtual u8 pending_interrupt_level() const = 0;
    // Vector number for an acknowledged interrupt. The Amiga autovectors.
    virtual u32 acknowledge_interrupt(u8 level) = 0;
    // Master time of the next scheduled hardware event, so the CPU only calls
    // back into the hardware when something is actually due.
    [[nodiscard]] virtual MasterTick next_event_time() const = 0;
};

enum class UnknownTargetPolicy : u8 {
    Interpret,  // development: log it, run the interpreter, keep going
    Fatal,      // release: a finished port must never reach one (rule 20)
};

struct RuntimeConfig {
    TimingMode timing = TimingMode::Instruction;
    UnknownTargetPolicy unknown_target = UnknownTargetPolicy::Interpret;
    bool trace_blocks = false;
    bool log_unknown_targets = true;
};

// Statistics a port needs in order to claim zero interpreter fallback.
struct RuntimeStats {
    u64 blocks_executed = 0;
    u64 interpreter_fallbacks = 0;
    u64 exceptions_taken = 0;
    u64 interrupts_taken = 0;
    std::vector<GuestAddr> unknown_targets;
};

class Runtime {
public:
    explicit Runtime(GuestMemory& memory, RuntimeConfig config = {});
    ~Runtime();

    [[nodiscard]] GuestMemory& memory() noexcept { return memory_; }
    [[nodiscard]] const GuestMemory& memory() const noexcept { return memory_; }
    [[nodiscard]] u32 address_mask() const noexcept { return memory_.address_mask(); }

    [[nodiscard]] BlockTable& blocks() noexcept { return blocks_; }
    [[nodiscard]] HookRegistry& hooks() noexcept { return hooks_; }
    [[nodiscard]] RuntimeConfig& config() noexcept { return config_; }
    [[nodiscard]] const RuntimeStats& stats() const noexcept { return stats_; }

    void set_hardware(HardwareModel* hw) noexcept { hardware_ = hw; }
    [[nodiscard]] HardwareModel* hardware() const noexcept { return hardware_; }

    // --- guest memory access, as called from generated code ---------------
    u8 read8(u32 a) { return memory_.read8(a); }
    u16 read16(u32 a) { return memory_.read16(a); }
    u32 read32(u32 a) { return memory_.read32(a); }
    void write8(u32 a, u8 v) { memory_.write8(a, v); }
    void write16(u32 a, u16 v) { memory_.write16(a, v); }
    void write32(u32 a, u32 v) { memory_.write32(a, v); }
    u16 fetch16(u32 a) { return memory_.fetch16(a); }

    // --- guest stack -------------------------------------------------------
    // The guest stack stays real: a translated JSR pushes a guest return
    // address to guest memory (AMIGA_RECOMP.md 8.3).
    void push16(M68kState& cpu, u16 v) {
        cpu.a[7] -= 2;
        memory_.write16(cpu.a[7], v);
    }
    void push32(M68kState& cpu, u32 v) {
        cpu.a[7] -= 4;
        memory_.write32(cpu.a[7], v);
    }
    u16 pop16(M68kState& cpu) {
        const u16 v = memory_.read16(cpu.a[7]);
        cpu.a[7] += 2;
        return v;
    }
    u32 pop32(M68kState& cpu) {
        const u32 v = memory_.read32(cpu.a[7]);
        cpu.a[7] += 4;
        return v;
    }

    // --- timing and interrupts --------------------------------------------
    void advance_cpu_cycles(M68kState& cpu, u32 cycles) {
        cpu.cycles += cycles;
        if (hardware_ && cpu.cycles >= hardware_deadline_) sync_hardware(cpu);
    }

    [[nodiscard]] bool interrupt_should_preempt(const M68kState& cpu) const noexcept {
        const u8 level = pending_level_;
        return level != 0 && (level == 7 || level > cpu.int_mask);
    }

    // Take the pending interrupt; returns the guest PC to continue at.
    u32 enter_pending_interrupt(M68kState& cpu, u32 next_pc);

    // --- exceptions --------------------------------------------------------
    // Group 1/2 exception (6-byte frame). Returns the new guest PC.
    u32 enter_exception(M68kState& cpu, Vector vector, u32 pc);
    u32 enter_exception_index(M68kState& cpu, u32 vector_index, u32 pc);
    // Group 0 fault: bus error or address error (14-byte frame).
    u32 enter_group0_fault(M68kState& cpu, const GuestFault& fault, u32 pc);

    // --- execution ---------------------------------------------------------
    // Run until the CPU halts or the cycle budget is exhausted. A budget of 0
    // means "no limit". Returns the number of blocks executed.
    u64 run(M68kState& cpu, u64 cycle_budget = 0);
    // Execute exactly one block (or one interpreted instruction on fallback).
    void step_block(M68kState& cpu);

    void note_unknown_target(GuestAddr address);

    // The guest address of the instruction currently executing. Translated
    // code records this before an access that can fault, so a bus or address
    // error can report the offending opcode in its exception frame. It is a
    // single store, and only on instructions that touch memory.
    void set_insn_pc(u32 pc) noexcept { insn_pc_ = pc; }
    [[nodiscard]] u32 insn_pc() const noexcept { return insn_pc_; }

    // Wired up only when the interpreter fallback is compiled in.
    using InterpretFn = u32 (*)(M68kState&, Runtime&);
    void set_interpreter(InterpretFn fn) noexcept { interpreter_ = fn; }
    [[nodiscard]] InterpretFn interpreter() const noexcept { return interpreter_; }

    void log(const char* fmt, ...);

private:
    void sync_hardware(M68kState& cpu);
    u32 run_block_at(M68kState& cpu, GuestAddr pc);

    GuestMemory& memory_;
    RuntimeConfig config_;
    BlockTable blocks_;
    HookRegistry hooks_;
    HardwareModel* hardware_ = nullptr;
    RuntimeStats stats_;
    InterpretFn interpreter_ = nullptr;

    u8 pending_level_ = 0;
    MasterTick hardware_deadline_ = 0;
    u32 insn_pc_ = 0;

    friend class Chipset;
};

} // namespace arecomp
