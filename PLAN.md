# PLAN.md — implementation plan & context handover

Living document. Read this first when resuming in a fresh context.

## What this repo is

An implementation of the specification in [AMIGA_RECOMP.md](AMIGA_RECOMP.md): a
**static recompiler** that translates Motorola 68000 machine code from Amiga
games into portable C++20, plus a native runtime (`AmigaRuntime`) that
implements the Amiga machine (Copper, blitter, bitplanes, sprites, Paula, CIA,
interrupts) around the translated CPU code.

[AMIGA.md](AMIGA.md) is the hardware / reverse-engineering reference.

It is **not** a decompiler. See AMIGA_RECOMP.md §0, §11, §67.

**End goal for this effort: widescreen support in selected games** — via
AMIGA_RECOMP.md §37 Level 1 (hardware-aware: wider fetch/display window) and
Level 2 (hook the game's own renderer/camera and drive it wider).

## Ground rules (from the user, not the spec)

1. Generated ports go in `projects/<id>/`, which is **git-ignored**. Never
   commit game-derived code or data.
2. Commit as work progresses, small and often.
3. Reference emulator (hardware behaviour, snapshot + trace capture):
   `git@github.com:retro-foundry/engine9000-public.git` — `ami9000/` is a
   libretro-UAE fork, `e9k-debugger/` is a 68k debugger/profiler.
   **Do not vendor it** (GPL, and we are MIT). Use it to *produce inputs*
   (snapshots, PC traces) and to check hardware behaviour.
   Note: cloning on Windows needs `git config --global core.longpaths true`.

## Toolchain present on this machine

`gcc 16.1.0 (MinGW-w64)`, `clang 22.1.5`, `cmake 4.0.0-rc4`, `python 3.13`,
`git 2.48`. No MSVC `cl` on PATH. Default build: Ninja + whatever CMake picks;
CI-ish builds use gcc. SDL is **optional** (`ARECOMP_WITH_SDL`); default
platform backend is headless so the whole tree builds here.

## Architecture in one screen

```
 BUILD TIME                             RUN TIME
 game image / snapshot                  generated .cpp blocks
   -> arecomp (tools/arecomp)             + libamiga_runtime
   -> decode 68000                        -> AmigaRuntime
   -> CFG of basic blocks                    memory / chipset / scheduler
   -> emit C++ per block                  -> platform backend (headless|SDL)
   -> projects/<id>/ CMake project
```

Generated block signature (the ABI everything hangs off):

```cpp
uint32_t block_00012000(M68kState& cpu, Runtime& rt);   // returns next guest PC
```

- Guest state is explicit (`M68kState`), guest stack is real, guest addresses
  are real. No host pointers leak into translated code.
- Hardware access always goes through `Runtime`, never raw memory.
- The dispatcher resolves a guest PC to a block; unknown target = interpreter
  fallback (dev) or fatal (release).

## Milestone status

Tracking AMIGA_RECOMP.md §66. Update this table as work lands.

| # | Milestone | State |
|---|-----------|-------|
| 0 | Skeleton: CMake, CLI, manifest, guest memory, logging, tests | not started |
| 1 | Minimal 68k recomp vertical slice (§69) | not started |
| 2 | Complete effective-address modes | not started |
| 3 | Full MC68000 user instruction set + randomized diff tests | not started |
| 4 | Exceptions / privilege / interrupts / RTE / STOP | not started |
| 5 | Amiga memory map, CIA + custom register decode, DMACON/INTENA, beam | not started |
| 6 | Video: palette, bitplanes, DIW/DDF, modulos, Copper, display out | not started |
| 7 | Blitter (area, minterms, fill, line, async busy/IRQ) | not started |
| 8 | Input / Paula audio / CIA timers | not started |
| 9 | Trace-assisted convergence, fallback removal | not started |
| 10 | Real game snapshot to gameplay, zero fallback | not started (needs game data) |
| 11 | Hooks / mod API + widescreen enhancement | not started |

## Layout

```
include/amiga_recomp/   public headers shared by runtime AND generated code
runtime/                AmigaRuntime: memory, cpu helpers, chipset, platform
tools/arecomp/          the recompiler CLI (decoder, CFG, translator, codegen)
tests/                  unit + differential tests
testroms/               synthetic 68k test programs (assembled by tools/asm68k.py)
examples/               small hand-written guest programs
docs/                   architecture, manifest, workflow, widescreen notes
projects/               GIT-IGNORED generated ports
```

## Key decisions taken

- **No external deps.** TOML is parsed by a small in-tree subset parser
  (`tools/arecomp/src/toml_lite.*`). SDL optional. Musashi *optional* oracle
  (`ARECOMP_WITH_MUSASHI`), not vendored.
- **Reference interpreter is in-tree** (`runtime/cpu/interpreter.cpp`) and
  shares the semantic helpers (`semantics.hpp`) with generated code. That means
  it validates *translation* (decode, EA, control flow, block stitching) but
  **not** the shared flag formulas — those are covered by hand-written unit
  tests and, when enabled, by the external Musashi oracle. Documented honestly
  rather than papered over.
- Decoder builds a 65536-entry opcode table from patterns at startup
  (Musashi-style), so decode is a table lookup, not a chain of ifs.
- One shared `ADVANCE(n)` path accounts guest cycles and gives the runtime an
  interrupt preemption point at every instruction boundary.
- 68000 `A7` byte accesses adjust by 2. Centralised in the EA layer.

## Widescreen approach (the actual goal)

See [docs/widescreen.md](docs/widescreen.md). Short version:

- Level 1 is generic and lands in the runtime: the chipset renders to a
  framebuffer wider than the DIW, the game's own Copper/bitplane program is
  re-evaluated across the wider fetch window, and `BPLxMOD` compensation is
  applied so the game's linear bitplane memory is interpreted at the wider
  pitch. This works only for games whose scroll/tile engine can be told to
  produce more columns — which needs Level 2.
- Level 2 is per-game and lands in `projects/<id>/patch/`: hooks on the game's
  camera / tile-column / BOB-clip routines, driven by the hook API (§35).
- The honest position: **no automatic widescreen.** The framework's job is to
  make the per-game work small, inspectable and testable.

## Next actions

1. Milestone 10 needs a real game snapshot — requires user-supplied game data
   and an ami9000 capture. Blocked on data, not on code.
2. Chip RAM contention timing (`TimingMode::ChipExact`) is stubbed to
   `BusAware`; only matters for raster-trick-heavy titles.
3. AGA / 68020 extension points exist but are unimplemented (§61, §62).
