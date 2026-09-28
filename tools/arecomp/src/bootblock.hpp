// Boot block analysis (AMIGA_RECOMP.md 4.3).
//
// A game that boots from its own boot block reads the rest of itself off the
// disk before jumping into it. Nothing can infer a trackloader in general, but
// the common case -- a sequence of trackdisk.device CMD_READ requests -- is a
// recognisable idiom, and reading it off the boot block is far more reliable
// than guessing.
//
// The output is advice, not truth: it reports what it recognised, disassembles
// the boot block so the reader can check, and says plainly when it found
// nothing.
#pragma once

#include <string>
#include <vector>

#include "amiga_recomp/types.hpp"

namespace arecomp {

struct BootLoad {
    u32 offset = 0;      // byte offset on the disk (io_Offset)
    u32 length = 0;      // bytes (io_Length)
    u32 address = 0;     // guest address (io_Data)
    u32 site = 0;        // where in the boot block the request was issued
};

struct BootblockReport {
    bool has_dos_signature = false;
    std::string signature;       // the four bytes, printable
    bool checksum_valid = false;
    std::vector<BootLoad> loads;
    bool found_entry_jump = false;
    u32 entry = 0;               // target of the final JMP, when there is one
    std::vector<std::string> disassembly;
    std::string note;            // why the analysis stopped, if it did
};

// Analyses the first 1024 bytes of a disk image.
bool analyse_bootblock(const std::vector<u8>& disk, BootblockReport& out,
                       std::string& error);

} // namespace arecomp
