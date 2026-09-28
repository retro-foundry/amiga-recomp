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
| 0 | Skeleton: CMake, CLI, manifest, guest memory, logging, tests | **done** |
| 1 | Minimal 68k recomp vertical slice (§69) | **done** |
| 2 | Complete effective-address modes | **done** |
| 3 | Full MC68000 user instruction set + differential tests | **done** (sweep-based; randomised fuzzing still to add) |
| 4 | Exceptions / privilege / interrupts / RTE / STOP | **done** for CPU side; interrupt sources arrive with the chipset |
| 5 | Amiga memory map, CIA + custom register decode, DMACON/INTENA, beam | **done** |
| 6 | Video: palette, bitplanes, DIW/DDF, modulos, Copper, display out | **done** (headless framebuffer; SDL backend pending) |
| 7 | Blitter (area, minterms, fill, line, async busy/IRQ) | **done** for area/fill/descending; line mode is approximate |
| 8 | Input / Paula audio / CIA timers | **done** (audio DMA + interrupts; no host mixer yet) |
| 9 | Trace-assisted convergence, fallback removal | not started |
| 10 | Real game snapshot to gameplay, zero fallback | **partly** - Vroom boots from ADF, decrunches, and the decrunched game recompiles and runs with zero fallback; it stops in its own level loader |
| 11 | Hooks / mod API + widescreen enhancement | **done** (hooks + level 1 widescreen; level 2 is per-game) |

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

## Vroom (the real game)

`ports/vroom.toml` boots the real disk image; `ports/vroom-game.toml`
recompiles the decrunched result. Neither the ADF nor the snapshot is in this
repository.

What works:

1. `arecomp bootblock` reads the loader's two `trackdisk.device` reads off the
   boot block and emits the manifest fragment.
2. The recompiled boot stub kills the OS, installs a level 1 vector, triggers a
   software interrupt, measures the CPU speed against a CIA timer, seeks the
   drive and reads raw MFM through the chipset's disk DMA.
3. It decrunches the game to `$3658`; the capture hook writes memory and
   machine state there.
4. That snapshot recompiles to **1361 blocks / 17 KB of game code**, which runs
   natively at about 5.6x Amiga speed with **zero interpreter fallbacks**.

Current state: **converged**. Runs 400 million guest cycles with no faults,
**31.7 million native blocks**, 1.75 million interpreter fallbacks, 2877
interrupts, 52+ disk reads, and **renders real graphics** (640x200, four
colours, structured image). `tools/converge.py` reports nothing further to
add: the only unresolved targets left are 104 addresses in $43xxx, which the
game loads from disk at run time and which are empty in the snapshot.

Four real bugs were found by getting this far, all now fixed:

1. **A 68000 decoder bug.** `cmpa.l a3,a6` ($bdcb) was decoded as
   `cmpm.l (a3)+,(a6)+`, because the CMPM pattern was matched without checking
   that the size field was a real size. That silently postincremented two
   registers the game only meant to compare, turning a loop-termination test
   into a runaway pointer. Regression test in tests/cpu/decoder_test.cpp.
2. **The wrong file in the drive.** A snapshot port passed argv[1] to
   insert_disk, so the game had its own 512 KB memory image in the floppy
   drive instead of the ADF.
3. **Chip state was never captured.** A snapshot is memory *and* machine state
   (section 39). Without the chip state the Copper resumed pointing at address
   zero, ran through low memory interpreting 68k code as a Copper list, and
   wrote `INTENA <- $4eb9`, clearing the master interrupt enable for good.
4. **The capture trigger did not force a block boundary**, so a trigger landing
   mid-block installed a hook that never fired.

Remaining: 64 unknown targets, all in $43xxx, which is **empty in the snapshot**
-- the game loads that code from disk at run time. Compiling it from the
snapshot translates zeros, which is the overlay case in section 31. It needs
either a later capture or per-region translation. Everything else runs
natively.

## Next actions

1. **Milestone 10, Vroom**: the game still sits in the wait loop at `$12f98`
   although the counter's incrementer at `$a4f8` is now compiled and its
   interrupts fire. Next: trace whether that path is actually taken, and
   handle the $43xxx overlay with a later capture.
2. **Milestone 10 generally.** Everything else is in place.
   Capture with ami9000 after the game has decrunched and taken over the
   machine (docs/decrunching.md), write a manifest, and converge the indirect
   targets until the fallback count is zero. Blocked on data, not on code.
2. ~~SDL platform backend.~~ Done: `runtime/platform/sdl_platform.cpp`, built
   with `-DARECOMP_WITH_SDL=ON`. Generated ports open a window, present each
   frame and map the arrow keys and space to joystick port 1. Without SDL the
   same source builds headless.
3. **OS shim generator** (`arecomp osshim`) reading the NDK `.fd` files.
   Format is understood, hook machinery exists. See docs/os_interaction.md.
   Only needed for OS-friendly games, which is a minority.
4. Audio has DMA and interrupts but no host mixer.
5. Blitter line mode is approximate; area mode is tested.
6. Chip RAM contention (`TimingMode::ChipExact`) is stubbed to `BusAware`.
7. AGA / 68020 extension points exist but are unimplemented (§61, §62).

## Demonstrations that currently work

- `ports/vertical-slice.toml` — the spec's §69 vertical slice, differentially
  tested against the interpreter.
- `ports/isa-sweep.toml` — 232 instructions of ISA coverage, differentially
  tested and pinned to hand-derived values.
- `ports/copper-demo.toml` — 68000 code programming the custom chips, run as
  a native executable with widescreen on: 9,500 native blocks, zero
  interpreter fallbacks, correct raster. Writes a PPM screenshot:

  ```sh
  arecomp recompile ports/copper-demo.toml -o projects/copper-demo
  cmake -S projects/copper-demo -B projects/copper-demo/build -G Ninja
  cmake --build projects/copper-demo/build
  projects/copper-demo/build/copper-demo       testroms/synthetic/copper_demo.bin frame.ppm
  ```
