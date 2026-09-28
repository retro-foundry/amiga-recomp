// MC68000 exception vectors and the fault-signalling mechanism used between
// the memory layer and the dispatcher (AMIGA_RECOMP.md 14).
//
// Faults that abort an instruction mid-way -- bus error and address error --
// are signalled with a C++ exception. They are rare by construction (a correct
// guest program raises none), so the cost is irrelevant, and generated blocks
// stay free of error plumbing on the hot path: a translated MOVE is a load, a
// store and a flag update, not a chain of status checks.
#pragma once

#include "amiga_recomp/types.hpp"

namespace arecomp {

enum class Vector : u8 {
    ResetSsp = 0,
    ResetPc = 1,
    BusError = 2,
    AddressError = 3,
    IllegalInstruction = 4,
    DivideByZero = 5,
    ChkInstruction = 6,
    TrapV = 7,
    PrivilegeViolation = 8,
    Trace = 9,
    LineA = 10,   // unimplemented instruction, opcode $Axxx
    LineF = 11,   // unimplemented instruction, opcode $Fxxx
    Spurious = 24,
    AutoVectorBase = 25,  // levels 1..7 occupy 25..31
    TrapBase = 32,        // TRAP #0..15 occupy 32..47
};

constexpr u32 vector_address(Vector v) noexcept {
    return static_cast<u32>(v) * 4u;
}
constexpr u32 vector_address(u32 index) noexcept { return index * 4u; }

// Group 0 fault (bus error / address error). These push the larger 68000
// exception frame and are thrown rather than returned.
struct GuestFault {
    Vector vector = Vector::AddressError;
    u32 address = 0;       // the offending guest address
    u16 opcode = 0;        // instruction register contents at the fault
    bool read = true;      // read cycle (vs write)
    bool instruction = false;  // the access was an instruction fetch
};

// A fatal condition the guest cannot recover from: double fault, or the
// dispatcher reaching an address with no compiled block in release mode.
struct FatalError {
    const char* reason;
    u32 address;
};

} // namespace arecomp
