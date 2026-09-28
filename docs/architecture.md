# Architecture

Implementation notes for [AMIGA_RECOMP.md](../AMIGA_RECOMP.md). This file
records what was built, and — more usefully — where the implementation
deviates from the specification and why.

## The two halves

```
 BUILD TIME                              RUN TIME

 image / snapshot / hunk                 generated .cpp blocks
   -> arecomp                              + libamiga_runtime
      decoder      (68000)                 -> Runtime
      analyzer     (basic blocks)             guest memory, dispatch, hooks
      codegen      (C++)                   -> Chipset
      project      (CMake port)               Copper, blitter, bitplanes,
   -> projects/<id>/                          sprites, Paula, CIAs
```

The CPU is recompiled. The custom chips are implemented. Neither half knows
anything game-specific: that lives in the manifest and in the port's `patch/`
directory.

## The generated-code ABI

Everything hangs off one signature:

```cpp
uint32_t block_00012000(M68kState& cpu, Runtime& rt);   // returns next guest PC
```

- Guest state is explicit and deterministic. Host CPU flags are never guest
  state.
- The guest stack is real: a translated `JSR` pushes a guest return address
  into guest memory, and `RTS` pops it. No host call stack is involved, so a
  game that manipulates its return address behaves as it did.
- Guest addresses stay guest addresses throughout translated control flow.
- Every hardware access goes through `Runtime`, never a raw pointer.

`ARECOMP_ADVANCE(cycles, next_pc)` closes every translated instruction: it
accounts guest time and gives the runtime an interrupt preemption point. That
is what keeps the CPU and the hardware advancing together instead of the CPU
running a whole frame and the chipset catching up afterwards.

## Deviations from the specification

Three, all deliberate.

### 1. Condition codes are separate fields, not packed into `sr`

The spec shows `uint16_t sr` holding everything. The implementation keeps
`x`, `n`, `z`, `v`, `c` as separate bytes, with `sr()` and `set_sr()` packing
and unpacking on demand for `MOVE to/from SR`, exceptions and `RTE`.

Guest state stays explicit, deterministic and independent of the host — every
rule the spec actually cares about. What changes is that translated code does
not pay a bitfield insert and extract on every arithmetic instruction, and the
flag helpers are far easier to get right and to unit-test.

### 2. Group 0 faults are signalled with a C++ exception

Bus error and address error abort an instruction part way through. Rather than
returning a status from every memory accessor and checking it everywhere,
`GuestMemory` throws `GuestFault` and the dispatcher catches it and builds the
68000 exception frame.

A correct guest program raises none of these, so the cost is irrelevant, and
generated blocks stay free of error plumbing on the hot path: a translated
`MOVE` is a load, a store and a flag update.

The instruction register field of the frame needs the opcode that faulted,
which the memory layer does not know. Translated code records the current
instruction address with `ARECOMP_INSN` before any access that can fault — a
single store, only on instructions that touch memory.

### 3. The reference interpreter shares the semantic helpers

`runtime/cpu/interpreter.cpp` calls the same `semantics.hpp` as generated
code. That means the differential tests validate **translation** — decode,
effective addresses, control flow, block stitching, cycle accounting — and
**not** the shared flag formulas, which would be wrong in both engines
identically.

That gap is covered two ways, and neither is optional:

- `tests/cpu/semantics_test.cpp` asserts flag behaviour directly against the
  MC68000 manual, including every edge case the spec calls out.
- `tests/integration/isa_sweep_test.cpp` asserts absolute results for a
  232-instruction program, derived by hand from the manual.

An external Musashi oracle can be enabled with `ARECOMP_WITH_MUSASHI` for a
fully independent check. It is not vendored.

## Timing

One master clock, in 68000 cycles at 7.09379 MHz (PAL). A colour clock is two
of those; a lores pixel is one; a CIA tick is ten. Host wall-clock time never
influences guest behaviour, so runs are reproducible.

`TimingMode::Instruction` (nominal instruction cycles) is implemented.
`BusAware` and `ChipExact` are declared and currently behave as
`Instruction`; Chip RAM contention matters only for raster-trick-heavy
software and is not on the path to a working game.

## Display

The beam walks each line one colour clock at a time and pixels are produced as
it goes, so a Copper write landing mid-line changes the pixels after it and
not the ones before. That is what raster bars and split palettes need.

Approximations, stated plainly:

- Bitplane DMA is modelled as one fetch per plane every eight colour clocks
  across the fetch window, not at Agnus's exact DMA slots.
- The fetch-to-display pipeline delay is absorbed by the shift registers
  rather than reproduced cycle for cycle.
- Geometry, scrolling, modulos and the display window are exact. Software that
  counts individual DMA slots is not a target.

The framebuffer is two columns per lores pixel, so hires loses no horizontal
resolution.

## Known gaps

- No SDL backend yet; the runtime renders to a framebuffer and a platform
  layer consumes it.
- Blitter line mode is approximate. Area mode, minterms, masks, shifts, fill
  and descending blits are tested.
- Audio implements DMA, buffer-exhausted interrupts and per-channel state, but
  no host mixer.
- Disk DMA and the ADF path are not implemented; see
  [decrunching.md](decrunching.md) for why snapshots are the primary route.
- AGA and 68020 have extension points but no implementation.
