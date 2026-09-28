// A minimal guest machine for CPU-level tests: RAM, a halt sentinel, and a
// runtime with the interpreter installed. No chipset, so these tests exercise
// the CPU and nothing else.
#pragma once

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <cstdio>
#include <string>
#include <vector>

#include "amiga_recomp/halt_device.hpp"
#include "amiga_recomp/interpreter.hpp"
#include "amiga_recomp/runtime.hpp"

namespace arecomp::test {

// A write here stops the harness. Synthetic test programs end with one
// (AMIGA_RECOMP.md 69: "halts through a test harness sentinel").
constexpr u32 kHaltAddress = 0x00f00000;
constexpr u32 kStackTop = 0x0007f000;
constexpr u32 kLoadAddress = 0x00001000;

struct Machine {
    M68kState cpu;
    GuestMemory memory{0x00ffffff};
    HaltDevice halt{cpu};
    Runtime runtime{memory};

    Machine() {
        memory.add_ram(0x000000, 0x080000, RegionKind::Chip, PERM_RWX, "chip");
        memory.map_device(kHaltAddress, GuestMemory::kPageSize, &halt,
                          RegionKind::Expansion, "halt-sentinel");
        cpu.pc = kLoadAddress;
        cpu.a[7] = kStackTop;
        cpu.s = true;
        cpu.int_mask = 7;
        // Tests deliberately run without compiled blocks, so the fallback
        // notice is expected and would only be noise.
        runtime.config().log_unknown_targets = false;
#if ARECOMP_ENABLE_INTERPRETER
        runtime.set_interpreter(&interpret_block);
#endif
        // An unhandled exception should land somewhere identifiable rather
        // than at address zero.
        for (u32 v = 0; v < 64; ++v) memory.write32(v * 4, 0x00ff0000);
    }

    bool load_file(const std::string& path, u32 address = kLoadAddress) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::vector<u8> data;
        u8 buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0)
            data.insert(data.end(), buffer, buffer + n);
        std::fclose(f);
        return memory.load(address, data.data(), data.size());
    }

    void load_words(std::initializer_list<u16> words, u32 address = kLoadAddress) {
        u32 a = address;
        for (u16 w : words) {
            memory.write16(a, w);
            a += 2;
        }
    }

    // Run to completion, with a bound so a runaway program fails the test
    // instead of hanging the suite.
    u64 run(u64 max_steps = 1000000) {
        u64 steps = 0;
        while (!cpu.halted && steps < max_steps) {
            runtime.step_block(cpu);
            ++steps;
        }
        return steps;
    }
};

inline std::string testrom_path(const char* name) {
    return std::string(ARECOMP_TESTROM_DIR) + "/" + name;
}

} // namespace arecomp::test
