// Capturing guest memory (AMIGA_RECOMP.md 39, docs/decrunching.md).
//
// A game that decrunches itself only exists as code once it has run. Writing
// guest memory out at that moment turns it into a snapshot the recompiler can
// translate, which is how a packed or disk-loaded game becomes a static port.
#pragma once

#include <string>

#include "amiga_recomp/cpu_state.hpp"
#include "amiga_recomp/guest_memory.hpp"

namespace arecomp {

// Writes `size` bytes of guest memory starting at `start` to a file. Unmapped
// addresses are written as zero rather than failing, so a capture spanning a
// hole in the address map still produces a usable image.
bool dump_memory(const GuestMemory& memory, u32 start, u32 size,
                 const std::string& path);

// Writes the machine state as a manifest fragment, so a captured image can be
// turned into a port by pasting it. A snapshot is memory *and* machine state;
// memory alone would start the game with the wrong registers.
bool dump_cpu_state(const M68kState& cpu, const std::string& path);

} // namespace arecomp
