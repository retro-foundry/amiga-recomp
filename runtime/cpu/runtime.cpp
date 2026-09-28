#include "amiga_recomp/runtime.hpp"

#include <cstdarg>
#include <limits>

namespace arecomp {

Runtime::Runtime(GuestMemory& memory, RuntimeConfig config)
    : memory_(memory), config_(config) {}

Runtime::~Runtime() = default;

void Runtime::log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

void Runtime::sync_hardware(M68kState& cpu) {
    hardware_->run_until(cpu.cycles);
    pending_level_ = hardware_->pending_interrupt_level();
    const MasterTick next = hardware_->next_event_time();
    hardware_deadline_ = next > cpu.cycles ? next : cpu.cycles + 1;
}

// ---------------------------------------------------------------------------
// Exception entry
// ---------------------------------------------------------------------------

namespace {

// Move to supervisor mode, capturing the previous SR first. Returns the SR
// value that belongs in the exception frame.
u16 enter_supervisor(M68kState& cpu) {
    const u16 old_sr = cpu.sr();
    if (!cpu.s) {
        cpu.usp = cpu.a[7];
        cpu.a[7] = cpu.ssp;
        cpu.s = true;
    }
    cpu.t1 = false;
    return old_sr;
}

} // namespace

u32 Runtime::enter_exception_index(M68kState& cpu, u32 vector_index, u32 pc) {
    const u16 old_sr = enter_supervisor(cpu);
    push32(cpu, pc);
    push16(cpu, old_sr);
    cpu.stopped = false;
    ++stats_.exceptions_taken;
    return memory_.read32(vector_address(vector_index)) & address_mask();
}

u32 Runtime::enter_exception(M68kState& cpu, Vector vector, u32 pc) {
    return enter_exception_index(cpu, static_cast<u32>(vector), pc);
}

u32 Runtime::enter_group0_fault(M68kState& cpu, const GuestFault& fault, u32 pc) {
    // A fault while building a fault frame is a double fault: a real 68000
    // halts, and so do we, loudly.
    static bool in_group0 = false;
    if (in_group0) {
        cpu.halted = true;
        log("[fatal] double fault at pc=%08x accessing %08x", pc, fault.address);
        return pc;
    }
    in_group0 = true;

    const u16 old_sr = enter_supervisor(cpu);

    // Function code: supervisor/user, program/data.
    u16 status = 0;
    const u16 fc = static_cast<u16>((cpu.s ? 4 : 0) | (fault.instruction ? 2 : 1));
    status |= fc & 7;
    if (!fault.instruction) status |= 1u << 3;   // I/N: 0 = instruction, 1 = not
    if (fault.read) status |= 1u << 4;           // R/W: 1 = read

    // The instruction register field: the fault itself may not know the
    // opcode, but the runtime knows which instruction was executing. peek16
    // is used so that recovering it can never fault a second time.
    const u16 opcode = fault.opcode ? fault.opcode : memory_.peek16(insn_pc_);

    push32(cpu, pc);
    push16(cpu, old_sr);
    push16(cpu, opcode);
    push32(cpu, fault.address);
    push16(cpu, status);

    cpu.stopped = false;
    ++stats_.exceptions_taken;
    in_group0 = false;
    return memory_.read32(vector_address(fault.vector)) & address_mask();
}

u32 Runtime::enter_pending_interrupt(M68kState& cpu, u32 next_pc) {
    const u8 level = pending_level_;
    const u32 vector_index =
        hardware_ ? hardware_->acknowledge_interrupt(level)
                  : static_cast<u32>(Vector::AutoVectorBase) + level - 1;

    const u16 old_sr = enter_supervisor(cpu);
    cpu.int_mask = level;               // masks this level and below
    push32(cpu, next_pc);
    push16(cpu, old_sr);
    cpu.stopped = false;
    ++stats_.interrupts_taken;

    // Taking the interrupt may have changed the mask; re-evaluate.
    if (hardware_) pending_level_ = hardware_->pending_interrupt_level();
    return memory_.read32(vector_address(vector_index)) & address_mask();
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void Runtime::note_unknown_target(GuestAddr address) {
    for (GuestAddr seen : stats_.unknown_targets)
        if (seen == address) return;
    stats_.unknown_targets.push_back(address);
    if (config_.log_unknown_targets)
        log("[recomp] no compiled block at %08x", address);
}

u32 Runtime::run_block_at(M68kState& cpu, GuestAddr pc) {
    if (BlockFn fn = blocks_.find(pc)) {
        ++stats_.blocks_executed;
        return fn(cpu, *this);
    }

    note_unknown_target(pc);

    if (config_.unknown_target == UnknownTargetPolicy::Fatal || interpreter_ == nullptr) {
        cpu.halted = true;
        throw FatalError{"no compiled block at guest address", pc};
    }

    // Development fallback: interpret a single instruction and try again. A
    // finished port must reach zero of these (AMIGA_RECOMP.md 64.9).
    ++stats_.interpreter_fallbacks;
    return interpreter_(cpu, *this);
}

void Runtime::step_block(M68kState& cpu) {
    const GuestAddr pc = cpu.pc;

    if (config_.trace_blocks)
        log("[block] %08x d0=%08x a7=%08x sr=%04x", pc, cpu.d[0], cpu.a[7], cpu.sr());

    const std::vector<Hook*>* attached = hooks_.at(pc);
    bool replaced = false;
    u32 next = pc;

    if (attached) {
        for (Hook* h : *attached) {
            if (!h->enabled || h->mode != HookMode::Before) continue;
            ++h->hits;
            h->fn(cpu, *this);
        }
        for (Hook* h : *attached) {
            if (!h->enabled || h->mode != HookMode::Replace) continue;
            ++h->hits;
            next = h->fn(cpu, *this);
            replaced = true;
            break;
        }
    }

    if (!replaced) next = run_block_at(cpu, pc);

    if (attached) {
        for (Hook* h : *attached) {
            if (!h->enabled || h->mode != HookMode::After) continue;
            ++h->hits;
            h->fn(cpu, *this);
        }
    }

    cpu.pc = next & address_mask();
}

u64 Runtime::run(M68kState& cpu, u64 cycle_budget) {
    const u64 start_blocks = stats_.blocks_executed;
    const u64 deadline = cycle_budget ? cpu.cycles + cycle_budget
                                      : std::numeric_limits<u64>::max();

    while (!cpu.halted && cpu.cycles < deadline) {
        if (cpu.stopped) {
            // STOP: guest time still passes, so hardware can raise the
            // interrupt that wakes us (AMIGA_RECOMP.md 56).
            if (!hardware_) break;
            advance_cpu_cycles(cpu, 4);
            if (interrupt_should_preempt(cpu))
                cpu.pc = enter_pending_interrupt(cpu, cpu.pc);
            continue;
        }

        try {
            step_block(cpu);
        } catch (const GuestFault& fault) {
            cpu.pc = enter_group0_fault(cpu, fault, cpu.pc);
        }
    }
    return stats_.blocks_executed - start_blocks;
}

} // namespace arecomp
