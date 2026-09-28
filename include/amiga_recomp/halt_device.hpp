// A test-harness sentinel: a guest write to a nominated address stops the CPU
// (AMIGA_RECOMP.md 69, "halts through a test harness sentinel").
//
// This is a debug facility, not part of the Amiga machine. A real port maps it
// only when its manifest asks for one, so that synthetic programs and replay
// harnesses have a defined way to say "done".
#pragma once

#include "amiga_recomp/cpu_state.hpp"
#include "amiga_recomp/guest_memory.hpp"

namespace arecomp {

class HaltDevice : public MmioDevice {
public:
    explicit HaltDevice(M68kState& cpu) : cpu_(cpu) {}

    u8 read8(u32) override { return 0; }
    u16 read16(u32) override { return 0; }
    void write8(u32, u8 value) override { halt(value); }
    void write16(u32, u16 value) override { halt(value); }

    // The value written is kept as an exit status, so a harness program can
    // report success or failure rather than only "it stopped".
    [[nodiscard]] u32 status() const noexcept { return status_; }

private:
    void halt(u32 value) {
        status_ = value;
        cpu_.halted = true;
    }

    M68kState& cpu_;
    u32 status_ = 0;
};

} // namespace arecomp
