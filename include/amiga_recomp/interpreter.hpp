// Reference interpreter entry points (AMIGA_RECOMP.md 27, 46).
//
// Compiled in only when ARECOMP_ENABLE_INTERPRETER is set, so a shipping port
// can prove it contains no interpreter at all (rule 18).
#pragma once

#include "amiga_recomp/types.hpp"

namespace arecomp {

class Runtime;
struct M68kState;

// Execute exactly one instruction at cpu.pc. Returns the next guest PC.
// Guest exceptions are entered through the runtime, so the returned PC may be
// an exception handler.
u32 interpret_one(M68kState& cpu, Runtime& rt);

// Matches the BlockFn signature so it can be installed as the fallback.
u32 interpret_block(M68kState& cpu, Runtime& rt);

} // namespace arecomp
