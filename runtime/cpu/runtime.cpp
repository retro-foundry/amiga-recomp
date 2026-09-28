#include "amiga_recomp/runtime.hpp"

#include <cstdarg>
#include <cstdlib>
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

// Reads an exception vector, complaining once if it is zero. A guest that
// runs off to address 0 is almost never doing so deliberately, and saying so
// turns a baffling crash into an obvious diagnosis.
u32 Runtime::check_vector(u32 vector_index, u32 pc) {
    const u32 handler = memory_.read32(vector_address(vector_index)) & address_mask();

    if (handler == 0 && !warned_null_vector_) {
        warned_null_vector_ = true;
        log("[runtime] vector %u (at $%08x) is zero; taken from pc=$%08x. "
            "Execution will continue from address 0.",
            vector_index, vector_address(vector_index), pc);
    }
    return handler;
}

u32 Runtime::enter_exception_index(M68kState& cpu, u32 vector_index, u32 pc) {
    const u16 old_sr = enter_supervisor(cpu);
    push32(cpu, pc);
    push16(cpu, old_sr);
    cpu.stopped = false;
    ++stats_.exceptions_taken;
    return check_vector(vector_index, pc);
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

    if (!warned_bus_error_) {
        warned_bus_error_ = true;
        log("[runtime] %s error: %s access to $%08x from pc=$%08x. That address "
            "is not mapped; the machine profile may be missing a memory region.",
            fault.vector == Vector::BusError ? "bus" : "address",
            fault.read ? "read" : "write", fault.address, pc);
    }

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
    return check_vector(static_cast<u32>(fault.vector), pc);
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

    const u32 handler = memory_.read32(vector_address(vector_index)) & address_mask();

    // Which interrupts a game actually takes, and where they go, is the first
    // thing to establish when it sits in a wait loop that never ends.
    static const bool trace_interrupts = std::getenv("ARECOMP_IRQ_TRACE") != nullptr;
    if (trace_interrupts && stats_.interrupts_taken <= 10) {
        log("[irq] level %u -> vector %u -> $%08x (resuming at $%08x)", level,
            vector_index, handler, next_pc);
    }

    // An enabled interrupt with no handler installed is almost always a port
    // problem rather than a game one, and it is invisible if the guest is
    // simply allowed to run off to address zero.
    if (handler == 0 && !warned_null_vector_) {
        warned_null_vector_ = true;
        log("[runtime] level %u interrupt taken through vector %u at $%08x, "
            "which is zero. The guest will run from address 0. Either the game "
            "had not installed its handler yet, or the captured state is from "
            "before it did.",
            level, vector_index, vector_address(vector_index));
    }
    return handler;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

namespace {
struct PageIndexBuilder {
    std::unordered_map<u32, std::vector<Runtime::CodeBlockRange>>* index;
    GuestMemory* memory;
};

void index_block(void* context, GuestAddr address, u32 length) {
    auto* builder = static_cast<PageIndexBuilder*>(context);
    if (length == 0) length = 2;   // at least one instruction
    // A block can straddle a page boundary, so record it on every page it
    // covers; a write to any of them retires it.
    const u32 last = address + length - 1;
    for (u32 page = address >> GuestMemory::kPageBits;
         page <= (last >> GuestMemory::kPageBits); ++page) {
        (*builder->index)[page].push_back({address, last});
        builder->memory->mark_code_page(page << GuestMemory::kPageBits);
    }
}

void on_code_write(void* context, u32 address) {
    static_cast<Runtime*>(context)->invalidate_code_at(address);
}
} // namespace

void Runtime::watch_translated_code() {
    blocks_by_page_.clear();
    PageIndexBuilder builder{&blocks_by_page_, &memory_};
    blocks_.for_each(&index_block, &builder);
    memory_.set_code_write_handler(&on_code_write, this);
}

void Runtime::invalidate_code_at(u32 address) {
    const u32 masked = address & address_mask();
    const u32 page = masked >> GuestMemory::kPageBits;
    auto it = blocks_by_page_.find(page);
    if (it == blocks_by_page_.end()) return;

    // Only the blocks whose own bytes were written. A game storing to a
    // variable that happens to share a page with code should not lose the
    // code: page granularity would retire hundreds of blocks for one store.
    std::size_t dropped = 0;
    std::vector<CodeBlockRange> survivors;
    survivors.reserve(it->second.size());
    for (const CodeBlockRange& block : it->second) {
        if (masked >= block.first && masked <= block.last) {
            blocks_.invalidate(block.first);
            ++dropped;
        } else {
            survivors.push_back(block);
        }
    }

    if (dropped == 0) {
        // Nothing of ours was hit. Re-arm the page so the next write is
        // checked too, otherwise one harmless store would blind us to a real
        // overwrite later.
        memory_.mark_code_page(masked);
        return;
    }

    ++stats_.code_overwrites;
    if (config_.log_unknown_targets) {
        log("[recomp] guest wrote over translated code at $%08x; dropped %zu "
            "block(s), which now fall back to the interpreter",
            masked, dropped);
    }

    if (survivors.empty()) blocks_by_page_.erase(it);
    else {
        it->second = std::move(survivors);
        memory_.mark_code_page(masked);   // the rest still need watching
    }
}

void Runtime::note_unknown_target(GuestAddr address) {
    for (GuestAddr seen : stats_.unknown_targets)
        if (seen == address) return;
    stats_.unknown_targets.push_back(address);
    if (config_.log_unknown_targets)
        log("[recomp] no compiled block at %08x (reached from %08x)", address,
            last_block_);
}

u32 Runtime::run_block_at(M68kState& cpu, GuestAddr pc) {
    if (config_.force_interpreter) {
        if (interpreter_ == nullptr) {
            cpu.halted = true;
            throw FatalError{"force_interpreter set but no interpreter linked", pc};
        }
        return interpreter_(cpu, *this);
    }
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
    if (!config_.force_interpreter && blocks_.find(pc)) last_block_ = pc;

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
            try {
                cpu.pc = enter_group0_fault(cpu, fault, cpu.pc);
            } catch (const GuestFault& nested) {
                // A fault while stacking a fault frame is a double fault. A
                // real 68000 halts; so do we, rather than letting the
                // exception escape and abort the process.
                log("[fatal] double fault at pc=%08x: %s access to %08x while "
                    "handling a fault at %08x",
                    cpu.pc, nested.read ? "read" : "write", nested.address,
                    fault.address);
                cpu.halted = true;
            }
        }
    }
    return stats_.blocks_executed - start_blocks;
}

} // namespace arecomp
