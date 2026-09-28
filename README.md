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
build/tools/arecomp/arecomp recompile ports/example/example.toml -o projects/example

# build and run the generated port
cmake -S projects/example -B projects/example/build -G Ninja
cmake --build projects/example/build
projects/example/build/example
```

Generated ports are written to `projects/`, which is git-ignored. **No game
data, Kickstart ROM or commercial binary belongs in this repository.** You must
supply your own legally obtained game data.

## Licence

MIT, see [LICENSE](LICENSE). Contains no Amiga ROM or game content.
