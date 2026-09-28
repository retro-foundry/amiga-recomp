# amiga-recomp

A **static recompiler** for Commodore Amiga games: it translates MC68000
machine code into portable C++20 ahead of time, and runs it against a native
runtime that implements the Amiga machine (Copper, blitter, bitplanes,
sprites, Paula, CIA, interrupts).

It is not a decompiler and not an emulator. The CPU is *recompiled*; the
custom chips are *implemented*.

```
 Amiga 68k machine code -> literal static translation -> generated C++
                        -> host compiler -> native executable
                        + AmigaRuntime (Amiga machine model)
```

- [AMIGA_RECOMP.md](AMIGA_RECOMP.md) — the architecture specification.
- [AMIGA.md](AMIGA.md) — Amiga hardware & reverse-engineering reference.
- [PLAN.md](PLAN.md) — implementation plan and current status.
- [docs/](docs/) — architecture, manifest format, porting workflow, widescreen.

## Why

To make old Amiga binaries run as controllable native applications, so that
ports can add modern windowing, controllers, high-resolution rendering and
**widescreen** on top of faithful original game logic.

Recompilation does not produce widescreen by itself. It produces a native
program where widescreen can be implemented. See [docs/widescreen.md](docs/widescreen.md).

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `ARECOMP_WITH_SDL` (SDL2 platform backend, default OFF),
`ARECOMP_WITH_MUSASHI` (external CPU oracle for differential tests, default
OFF), `ARECOMP_BUILD_TESTS` (default ON).

## Use

```sh
# translate a guest image into a native project
build/bin/arecomp recompile ports/copper-demo.toml -o projects/copper-demo

# build and run the generated port
cmake -S projects/copper-demo -B projects/copper-demo/build -G Ninja
cmake --build projects/copper-demo/build
projects/copper-demo/build/copper-demo     testroms/synthetic/copper_demo.bin frame.ppm
```

`arecomp analyze <manifest>` reports what discovery found and what it could
not resolve, without emitting anything.

## Status

Working, with synthetic programs rather than a real game:

- **CPU**: the full MC68000 user instruction set and every addressing mode,
  decoded, translated and differentially tested against a reference
  interpreter, with absolute results pinned to the Motorola manual.
- **Chipset**: Copper, bitplane display, blitter, sprites, CIA timers, Paula
  DMA and the interrupt controller, driven by one master clock.
- **Widescreen**: level 1 window extension, working and tested.
  See [docs/widescreen.md](docs/widescreen.md) for what it can and cannot do.
- **Demonstration**: `ports/copper-demo.toml` is 68000 code that programs the
  custom chips, recompiled to native code. It runs 9,500 blocks with **zero
  interpreter fallbacks** and renders a correct raster.

Not done: an SDL backend (the runtime renders to a framebuffer; only a PPM
writer consumes it), a host audio mixer, the ADF/disk path, an OS shim, and
a real game. See [PLAN.md](PLAN.md).

Generated ports are written to `projects/`, which is git-ignored. **No game
data, Kickstart ROM or commercial binary belongs in this repository.** You must
supply your own legally obtained game data.

## Licence

MIT, see [LICENSE](LICENSE). Contains no Amiga ROM or game content.
