# Porting a game

The workflow from AMIGA_RECOMP.md §63, as it actually works with this tool.

## 0. Legal data only

You supply the game. Nothing in this repository contains, embeds or ships game
data, and generated ports live in `projects/`, which is git-ignored. Keep it
that way.

## 1. Get an image the recompiler can see

Read [decrunching.md](decrunching.md) first. Most commercial games are packed
or use a trackloader, and a snapshot taken after the game has decrunched and
taken over the machine is usually the shortest path to a working port.

## 2. Write a manifest

See [manifest.md](manifest.md). Start minimal: input, entry, memory regions.

## 3. Look before you translate

```sh
arecomp analyze ports/<id>.toml
```

This reports blocks, instructions, byte coverage and — the number that
matters — unresolved indirect transfer sites. If coverage is a few hundred
bytes, you are looking at a depacker rather than the game.

## 4. Translate

```sh
arecomp recompile ports/<id>.toml -o projects/<id>
cmake -S projects/<id> -B projects/<id>/build -G Ninja
cmake --build projects/<id>/build
```

`build_report.txt` in the port lists what was translated and what still needs
resolving.

## 5. Converge

Run with the interpreter fallback enabled. Every address the dispatcher
reaches with no compiled block is recorded. Add the ones that are real code to
the manifest as `[[indirect_target]]` entries or `[[code.range]]` extensions,
and regenerate.

Repeat until the fallback count is zero. That is the definition of a finished
port (AMIGA_RECOMP.md §64.9), and it is what lets you build with
`ARECOMP_ENABLE_INTERPRETER=OFF` and know the game is running entirely on
statically generated code.

## 6. Validate

Compare against the reference: same input replay, same frames, same audio,
same final state. `tests/differential/` shows the pattern.

The port carries its own reference inside it. `ARECOMP_INTERPRET=1` runs the
guest through the interpreter instead of the compiled blocks, and the two must
produce identical state. When they do not, `ARECOMP_DIGEST=N` prints a running
digest of guest registers every N blocks, sampled only where a compiled block
begins so that both engines sample the same points:

```sh
ARECOMP_DIGEST=1000 projects/<id>/build/<id> <image> a.ppm 60000000 2> a.log
ARECOMP_INTERPRET=1 ARECOMP_DIGEST=1000 projects/<id>/build/<id> <image> b.ppm 60000000 2> b.log
diff <(grep digest a.log) <(grep digest b.log) | head
```

The first differing line names the block after which they parted company.
Narrow it with `ARECOMP_DIGEST=1` over that window and you have the exact
block to look at. This is how the bug that drew Vroom's title screen in four
colours instead of sixteen was found: it came down to a single `move.l` that
patched two adjacent one-instruction blocks, only the first of which was being
retired.

`Runtime::memory().set_write_watch()` answers the other half of the question --
which code filled this buffer, and why did it stop -- by reporting every guest
write into an address range, DMA included.

## 6a. Play it

Build the port with the SDL backend and it opens a window:

```sh
cmake -S projects/<id> -B projects/<id>/build-sdl -G Ninja       -DARECOMP_WITH_SDL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build projects/<id>/build-sdl
projects/<id>/build-sdl/<id> <image>
```

Arrow keys and Space (or Left Ctrl) drive joystick port 1, which is where a
game looks for a joystick. The mouse drives port 0, which is what most menus
want. Escape quits, F11 toggles fullscreen. The picture keeps the Amiga's
pixel aspect and is fitted to the window.

A gamepad drives the same two ports, and is picked up whether it was plugged
in before the port started or during play:

| Gamepad | Amiga |
|---|---|
| D-pad, left stick | Joystick port 1 directions |
| A / B / X / Y, right shoulder, right trigger | The stick's one fire button |
| Right stick | Mouse pointer on port 0 |
| Left shoulder, left trigger | Left mouse button |

Host keys also reach the Amiga keyboard itself, as raw key codes through
CIA-A's serial register, for the games that read it rather than a joystick.

Keyboard and gamepad feed the same port rather than one each, so either can
be picked up mid-game and holding both is not a conflict. The right stick is
mapped to the mouse because a game whose menu wants a pointer would otherwise
be unreachable from the controller it is played with.

Without SDL the same port builds headless and writes a PPM of its last frame
instead, which is what the tests and replay harnesses use:

```sh
projects/<id>/build/<id> <image> frame.ppm 400000000
```

The third argument is a guest cycle budget, so a port that hangs during
bring-up stops instead of spinning.

## 7. Then, and only then, enhance

Hooks, modern input, and [widescreen.md](widescreen.md). Enhancements layered
on an incorrect port are not worth debugging.
