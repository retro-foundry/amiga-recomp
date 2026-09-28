# Crunchers, trackloaders and where the code actually is

The single biggest practical obstacle to statically recompiling an Amiga game
is that, on disk, **the game is not code**. It is compressed data behind a
depacker, or raw MFM behind a custom trackloader, or both.

Static discovery from the entry point finds the depacker stub, decodes a few
dozen instructions, and then walks into a wall of compressed payload that
decodes as garbage. That is not a bug in the recompiler; there is genuinely no
68k code there yet.

## What you are up against

**Crunchers.** Almost every commercial Amiga release is packed. PowerPacker
(`PP20`), Imploder, RNC ProPack, ByteKiller, Crunch-Mania, Titanics Cruncher,
TurboImploder and dozens more. Some are file crunchers producing a normal hunk
executable that unpacks itself; some pack individual data files; some pack the
whole disk.

**Trackloaders.** Most games abandon AmigaDOS entirely and read raw MFM from
the floppy with their own loader, often with copy protection woven into the
track format. Code arrives at addresses that did not exist in the initial
image, sometimes overlaid on top of earlier code.

**Self-modifying code.** Decrunchers are the most common source: they write
executable code into Chip RAM and then jump to it.

## Three ways through, in the order to try them

### 1. Snapshot after decrunch (the primary route)

This is why AMIGA_RECOMP.md makes Mode A the MVP rather than treating it as a
shortcut. Let the real machine run the loader, the depacker and the OS
takeover, then capture memory once the game is resident and running.

From that snapshot there is no compression, no trackloader and usually no
operating system dependency left — the game has already taken over the
hardware. One capture solves the decrunch problem, the Kickstart problem and
the protection problem at the same time.

Capture with `ami9000` from the
[engine9000](https://github.com/retro-foundry/engine9000-public) tree (a
libretro-UAE fork) and its `e9k-debugger`. What the manifest needs:

```toml
[input]
type = "snapshot"
memory = "capture/memory.bin"

[cpu]
entry = 0x00012000
sr = 0x2700
a = [0, 0, 0, 0, 0, 0, 0, 0x0007ff00]

[[code.range]]
start = 0x00012000
end   = 0x00026000
```

Capture at a **stable, repeatable point** — the top of the main loop, just
after a `VERTB` wait, is ideal. Avoid capturing mid-blit or mid-DMA.

The limitation is honest and worth stating: the port starts from the captured
state. It does not boot from the title screen unless the snapshot does.

### 2. Recompile the depacker as well

A depacker is ordinary 68000 code. Translate it, let it run, and it writes the
real game into Chip RAM — at which point you have code that did not exist at
build time. That is the overlay and self-modifying-code problem
(AMIGA_RECOMP.md §31, §32), and it is handled by convergence rather than
cleverness:

1. Run with the interpreter fallback enabled.
2. The runtime reports every address it reached with no compiled block.
3. Dump the regions that were written and then executed.
4. Feed them back to `arecomp` as additional code ranges and entry points.
5. Repeat until the set of discovered code stops growing and the fallback
   count reaches zero.

This is the same loop as resolving indirect jump targets, and it uses the same
machinery: `arecomp analyze` reports what it could not resolve,
`[[indirect_target]]` and `[[code.range]]` record what a run discovered.

Worth it when you want the port to start from the real entry point, or when
the game decrunches levels on the fly during play. Note that a game which
decrunches *new* code during play cannot be fully statically recompiled ahead
of time; those regions need either a capture that includes them or a
per-region translation pass.

### 3. Decrunch offline

For known formats, unpack ahead of time and recompile the plain image. This is
the cleanest result when it applies: no capture, no convergence loop, and the
port starts at the real entry point.

It requires identifying the packer and having a decompressor for it. The
common formats are well documented and several have permissively licensed
implementations. This is the right approach for a title packed with a
standard file cruncher and otherwise OS-friendly.

## What the framework does about it

- `arecomp analyze` stops discovery at undecodable opcodes and says so, rather
  than emitting translations of compressed data. If discovery dies a few
  hundred bytes past the entry point, you are looking at a depacker.
- Unresolved computed jumps are reported, not guessed.
- The runtime records every unknown target it reaches
  (`RuntimeStats::unknown_targets`), which is the raw material for the
  convergence loop.
- `[[code.range]]`, `[[indirect_target]]` and `[code] entry_points` in the
  manifest are how discovered regions are recorded. They are metadata, not
  reconstructed source.

## Diagnosing which one you have

| Symptom | Likely cause |
|---|---|
| Discovery stops within a few hundred bytes of the entry point | file cruncher |
| Entry point is in Chip RAM below $400 | bootblock, so trackloader |
| Discovery covers a small region that writes a much larger one, then jumps into it | depacker |
| Hunk executable, discovery covers most of it | OS-friendly, no crunching |
| Discovery stops at a `jmp (a0)` | unresolved indirect, not compression |

The last row matters: an unresolved indirect jump looks like a wall but is not
one. Add the target to the manifest and discovery continues.
