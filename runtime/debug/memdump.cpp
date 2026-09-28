#include "amiga_recomp/memdump.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace arecomp {

bool dump_memory(const GuestMemory& memory, u32 start, u32 size,
                 const std::string& path) {
    if (size == 0) return false;

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;

    std::vector<u8> buffer(65536);
    u32 written = 0;
    while (written < size) {
        const u32 chunk = std::min<u32>(static_cast<u32>(buffer.size()), size - written);
        for (u32 i = 0; i < chunk; ++i) buffer[i] = memory.peek8(start + written + i);
        std::fwrite(buffer.data(), 1, chunk, file);
        written += chunk;
    }

    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool dump_cpu_state(const M68kState& cpu, const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "w");
    if (!file) return false;

    std::fprintf(file, "# Captured machine state. Paste into a manifest.\n");
    std::fprintf(file, "[cpu]\n");
    std::fprintf(file, "entry = 0x%08x\n", cpu.pc);
    std::fprintf(file, "sr = 0x%04x\n", cpu.sr());

    std::fprintf(file, "d = [");
    for (int i = 0; i < 8; ++i)
        std::fprintf(file, "0x%08x%s", cpu.d[i], i == 7 ? "]\n" : ", ");

    std::fprintf(file, "a = [");
    for (int i = 0; i < 8; ++i)
        std::fprintf(file, "0x%08x%s", cpu.a[i], i == 7 ? "]\n" : ", ");

    std::fprintf(file, "# usp = 0x%08x, ssp = 0x%08x\n", cpu.usp, cpu.ssp);

    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

} // namespace arecomp
