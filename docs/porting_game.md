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

## 7. Then, and only then, enhance

Hooks, modern input, and [widescreen.md](widescreen.md). Enhancements layered
on an incorrect port are not worth debugging.
