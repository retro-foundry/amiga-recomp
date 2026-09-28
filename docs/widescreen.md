# Widescreen

How to get a wider picture out of a recompiled Amiga game, what the hardware
will and will not give you, and where the per-game work actually is.

The short version: **static recompilation does not produce widescreen.** It
produces a native program in which widescreen can be implemented. There are
two mechanisms, and only one of them is generic.

## The three levels

Following AMIGA_RECOMP.md §37:

| Level | What it is | Where it lives |
|-------|-----------|----------------|
| 0 | Faithful Amiga output, scaled to any window | runtime, automatic |
| 1 | Hardware-aware: widen the fetch and display windows | runtime, `WidescreenConfig` |
| 2 | Replace the game's renderer with a native one | per-game, `patch/` |

## Level 1: reveal what the game already draws

Most Amiga side-scrollers render into a bitmap **wider than the screen**. They
have to: smooth horizontal scrolling needs somewhere to put the column that is
about to scroll in. A typical setup is a 352- or 384-pixel-wide bitmap showing
a 320-pixel window, with `BPLCON1` doing the fine scroll and a positive
`BPL1MOD` absorbing the difference.

Those spare columns are drawn every frame and never displayed. Level 1 shows
them, and it needs three things to be right at once:

1. **Widen the fetch window.** `DDFSTRT` moves left and `DDFSTOP` moves right,
   in whole fetch blocks — one word per plane, so 16 lores pixels at a time.
   Asking for 40 extra pixels gets you 32.

2. **Walk the bitplane pointers back.** Extending the fetch window to the left
   does not by itself move where each row starts. Without subtracting the
   extra words from every `BPLxPT` at the start of the frame, the left margin
   shows columns that belong on the right, and the picture tears.

3. **Compensate the modulo.** Fetching more words per line advances each
   pointer further per line. Subtracting the extra fetch from `BPLxMOD` keeps
   the row stride the game intended. Without this the image shears by a few
   words per scan line — which is what
   `tests/chipset/widescreen_test.cpp` deliberately demonstrates.

Turning it on:

```cpp
auto& ws = chipset.widescreen();
ws.enabled = true;
ws.extra_left = 32;        // lores pixels
ws.extra_right = 32;
ws.compensate_modulo = true;
```

Ask the hardware what is actually available rather than guessing:

```cpp
const int slack = chipset.slack_words(0);   // words of BPL1MOD headroom
// slack * 16 is how many lores pixels the game's bitmap has spare.
```

And check what you actually got, because a request is not a promise:

```cpp
chipset.applied_extra_left();    // after block rounding and raster clamping
chipset.applied_extra_right();
```

### The limit nobody can design around

A PAL scan line is 227 colour clocks, which is **454 lores pixels**, and about
85 of those are horizontal blanking. The usable raster is therefore roughly
**370 lores pixels**.

The standard Amiga display window sits at lores 129 to 449. That is 320 pixels
with about 40 pixels of room on the left and **five on the right**. A game
using the standard window can be widened a little, and mostly leftwards.

So Level 1 realistically takes you from 320 to somewhere around 352–368
pixels: from 4:3 to about 1.35:1. That is overscan, and it is worth having,
but it is **not 16:9**. 16:9 from a 320-pixel 4:3 image would need about 427
lores pixels, and the raster does not contain them.

Games that use a narrower window — 256 pixels is common — have far more room,
and for those Level 1 alone can be a genuine widescreen win.

### When Level 1 is the wrong tool

- The bitmap is exactly as wide as the display window (`slack_words()` returns
  0). There is nothing to reveal. Widening will show adjacent rows.
- The game culls sprites, enemies or tile updates at the original screen edge.
  You will see the extra columns, and they will be empty or full of objects
  popping in. This is the usual outcome, and it is why Level 2 exists.
- The game's HUD is positioned relative to the original window.

## Level 2: hook the game's own renderer

This is where real widescreen remasters come from, and it is per-game work.
The recompiled 68k game logic keeps running exactly as it did; native code
takes over the parts that decide *what* is drawn and *how far*.

The hook API (`include/amiga_recomp/hooks.hpp`, AMIGA_RECOMP.md §35) attaches
native functions to guest addresses. The recompiler forces a basic-block
boundary at every hook site, so the dispatcher can run native code before,
after, or instead of the original block.

```cpp
// patch/hooks.cpp in the generated port
runtime.hooks().add(0x0001a820, HookMode::Replace, "tile_column_renderer",
                    [](M68kState& cpu, Runtime& rt) -> uint32_t {
                        // Draw the column the game asked for, then the extra
                        // ones its culling would have skipped.
                        draw_tile_column(cpu.d[0], rt);
                        draw_tile_column(cpu.d[0] - 1, rt);
                        draw_tile_column(cpu.d[0] + 20, rt);
                        return rt.pop32(cpu);   // return as the original would
                    });
```

The routines worth finding, in roughly the order they pay off:

1. **The tile/column renderer.** Call it for the extra columns each side. This
   alone turns a sheared Level 1 picture into a correct one.
2. **Object culling.** Games skip updating or drawing anything outside the
   original 320-pixel window. Widen the test and the extra columns come alive.
3. **The camera.** Clamps that keep the view inside the level need widening,
   or the player can walk off the edge of the world.
4. **The HUD.** Usually wants repositioning rather than widening.

[AMIGA.md](../AMIGA.md) has the reverse-engineering heuristics for finding
these: trace bitplane pointers backwards, find the blitter scheduler, work
from source pointers into resources.

## Recommended workflow for a game

1. Get the game running natively with zero interpreter fallback first.
   Widescreen on top of an incorrect port is not worth debugging.
2. Enable the register log (`ChipsetConfig::log_register_writes`) and look at
   what the game does with `DDFSTRT`, `DDFSTOP`, `DIWSTRT`, `DIWSTOP`,
   `BPLCON1` and `BPLxMOD` for one frame.
3. Read `slack_words()`. If it is 0, go straight to Level 2.
4. Turn on Level 1 with the slack you actually have. Expect empty or
   glitching margins — that is the culling, not a bug in the framework.
5. Find the column renderer and hook it. Then the culling test. Then the
   camera clamp.
6. Keep the original behaviour available behind a switch, so the faithful
   rendering stays testable against the recompiled logic.

## What this framework will not do

It will not infer any of the above. There is no analysis that reliably finds a
tile renderer in an arbitrary 68k binary, and a framework that guessed would
produce ports that are subtly wrong in ways nobody notices until late. The
job here is to make the per-game work small, inspectable and testable — not to
pretend it is unnecessary.
