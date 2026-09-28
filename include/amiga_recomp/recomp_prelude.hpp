// Everything a generated translation unit needs, and nothing else
// (AMIGA_RECOMP.md 29, 43).
//
// Generated code is a literal translation of 68000 machine semantics. It
// names guest registers and guest addresses, it calls the shared semantic
// helpers, and it never sees a host pointer.
#pragma once

#include "amiga_recomp/cpu_state.hpp"
#include "amiga_recomp/dispatch.hpp"
#include "amiga_recomp/exceptions.hpp"
#include "amiga_recomp/runtime.hpp"
#include "amiga_recomp/semantics.hpp"
#include "amiga_recomp/types.hpp"

namespace arecomp::generated {

using namespace arecomp::sem;

} // namespace arecomp::generated

// Account guest time and give the runtime a chance to interrupt at this
// instruction boundary (AMIGA_RECOMP.md 15). Every translated instruction
// passes through here, so hardware and CPU advance together instead of the
// CPU running a whole frame and the hardware catching up afterwards.
//
// `next_pc` is where the guest resumes if the interrupt is taken: the address
// after this instruction, or, for a control transfer, the address it
// transferred to, since by this point the instruction has completed.
#define ARECOMP_ADVANCE(cycles, next_pc)                                     \
    do {                                                                     \
        rt.advance_cpu_cycles(cpu, (cycles));                                \
        if (rt.interrupt_should_preempt(cpu))                                \
            return rt.enter_pending_interrupt(cpu, (next_pc));               \
    } while (0)

// Record which instruction is executing, so a bus or address error can name
// the opcode responsible. Emitted only before instructions that touch memory.
#define ARECOMP_INSN(pc) rt.set_insn_pc(pc)
