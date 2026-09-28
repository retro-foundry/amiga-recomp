# Hooks and native replacement

The mechanism every enhancement is built on (AMIGA_RECOMP.md §35, §36).

A hook attaches a native function to a guest address. The recompiler forces a
basic-block boundary there, so the dispatcher can run native code before,
after, or instead of the original block.

```cpp
runtime.hooks().add(0x0001a820, HookMode::Before, "log_frame",
                    [](M68kState& cpu, Runtime& rt) -> uint32_t {
                        std::printf("frame %u\n", cpu.d[0]);
                        return cpu.pc;       // ignored for Before/After
                    });
```

| Mode | Behaviour |
|---|---|
| `Before` | native code runs, then the original guest block |
| `After` | the original guest block runs, then native code |
| `Replace` | native code runs instead, and returns the next guest PC |

A `Replace` hook is a native function replacement. It must leave guest state
as the original code would: if the original ended in `RTS`, return
`rt.pop32(cpu)`.

## Rules worth respecting

- Hooks see and modify real guest state. Read arguments from `cpu.d[]` and
  `cpu.a[]`, and guest memory through `rt.read*` / `rt.write*`, never a host
  pointer.
- Declare hook addresses in the manifest so the block boundary exists. A hook
  at an address inside a block will never fire.
- Keep the original path available behind a switch
  (`HookRegistry::set_enabled`), so faithful behaviour stays testable.
- `Hook::hits` counts invocations, which is the quickest way to find out that
  the routine you hooked is not the one the game actually calls.

## Driving a game during bring-up

A hook is the simplest way to answer "is it stuck, or is it waiting for me?".
A game idling on its title screen and a game wedged in a loop look identical
from outside; a hook on the loop tells them apart, and can then supply the
input that moves it on.

```cpp
// The title screen's per-frame wait: it spins until the interrupt handler
// changes the frame counter. Watching that counter says whether the game is
// stuck or idling, and after a while we press fire to find out if it moves on.
runtime.hooks().add(0x00012f98, HookMode::Before, "title-probe",
    [](M68kState& cpu, Runtime& rt) -> uint32_t {
        static u32 visits = 0;
        const u32 counter = rt.memory().peek32(0x0000a884);
        if (++visits == 400) {
            if (Chipset* chipset = port::active_chipset())
                chipset->set_joystick(1, false, false, false, false, true);
        }
        return cpu.pc;
    });
```

That exact hook is how Vroom was shown to be running rather than hung: the
counter was advancing all along, and pressing fire moved it from the title
screen to the menu.

Hooks live in the generated port's `patch/` directory, which arecomp writes
once and never overwrites, so they survive regenerating the translation.

## What hooks are for

Modern controller mapping, debug overlays, save states, high-resolution
renderers, and [widescreen](widescreen.md). The generic recompiler infers none
of it; the hook API is what makes it possible.
