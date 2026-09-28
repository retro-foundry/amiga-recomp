# The game manifest

Every port has one TOML file describing the game. This is where all
game-specific knowledge lives: **metadata, not reconstructed source**
(AMIGA_RECOMP.md §5).

The parser is a small in-tree TOML subset (`tools/arecomp/src/toml_lite.*`).
It accepts decimal, `0x` and Amiga `$` hex, and reports a line number on any
malformed line rather than quietly defaulting.

## Minimal

```toml
[game]
name = "Example"
id = "example"
machine = "a500-pal"

[input]
type = "flat"              # flat | snapshot | hunk | adf
path = "../data/example.bin"
load_address = 0x00001000

[cpu]
entry = 0x00001000
sr = 0x2700
```

## Everything

```toml
[game]
name = "Example"
id = "example-pal"
machine = "a500-pal"
sha256 = "..."             # recorded, not enforced

[input]
type = "snapshot"
memory = "capture/memory.bin"

[cpu]
entry = 0x00012000
sr = 0x2700
d = [0, 0, 0, 0, 0, 0, 0, 0]
a = [0, 0, 0, 0, 0, 0, 0, 0x0007ff00]

[video]
standard = "pal"           # pal | ntsc

[recompiler]
strict_address_errors = true
development_interpreter_fallback = true
emit_instruction_comments = true
emit_cycle_accounting = true
blocks_per_file = 256      # translation units, for compile parallelism

[harness]
halt_address = 0x00f00000  # debug sentinel; omit for a real game

[[memory.region]]
start = 0x000000
size = 0x080000
kind = "chip"              # chip | fast | slow | rom | expansion
permissions = "rwx"

[[code.range]]
start = 0x00012000
end   = 0x00026000

# A computed JMP/JSR whose targets a trace run observed.
[[indirect_target]]
site = 0x000154ac
targets = [0x00016000, 0x00016124]

# Replace an instruction before translation.
[[patch.instruction]]
address = 0x00018022
replacement = "nop"

# Force a block boundary so native code can intercept here.
[[hook]]
address = 0x0001a820
name = "native_read_controls"
mode = "replace"           # before | after | replace
```

## Notes

- `[[code.range]]` constrains discovery. With none declared, discovery is
  unconstrained and will happily walk into data.
- `[[hook]]` entries generate commented stubs in `patch/hooks.cpp` on first
  generation; the implementation is yours and is never overwritten.
- `[harness]` exists for synthetic test programs and replay harnesses. A real
  port omits it and carries no test scaffolding.
