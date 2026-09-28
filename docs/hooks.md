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

## What hooks are for

Modern controller mapping, debug overlays, save states, high-resolution
renderers, and [widescreen](widescreen.md). The generic recompiler infers none
of it; the hook API is what makes it possible.
