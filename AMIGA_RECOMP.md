# AMIGA_RECOMP.md — Static Recompiler Architecture for Amiga Games

## 0. Purpose

Implement a reusable **static recompilation framework for Commodore Amiga games**.

This project is **not a decompiler**.

It must not attempt to reconstruct high-level game source, infer C structs, recover variable names, or turn 68k assembly into idiomatic C/C++.

The core job is:

```text
original Amiga 68k machine code
        ↓
literal static translation
        ↓
generated portable C++
        ↓
normal host compiler
        ↓
native x86-64 / ARM64 executable
```

The generated program retains:

- Motorola 68000 register state.
- original guest addresses.
- original big-endian memory semantics.
- the original Amiga hardware register interface.
- original interrupt vectors and stack behaviour.
- original Copper/blitter/bitplane/Paula programming model.

A separate native runtime implements the Amiga machine environment around the recompiled CPU code.

The design is deliberately similar in philosophy to N64Recomp:

> translate machine instructions literally and preserve the original machine model; do not recover source code.

The immediate purpose is to make old Amiga binaries run as controllable native applications so that ports and remasters can later add:

- modern windowing;
- modern controller input;
- integer scaling;
- aspect correction;
- high-resolution replacement renderers;
- widescreen patches where the game permits it;
- native audio backends;
- debug overlays;
- function replacement;
- hooks and mods;
- platform-independent builds.

None of those enhancements is automatically produced by recompilation. The recomp provides a stable native execution environment in which they can be implemented.

---

# 1. Scope

## 1.1 Initial target

The first production target is:

```text
CPU:        Motorola MC68000
Chipset:    OCS, with ECS compatibility where cheap
Machine:    A500-class
Video:      PAL first, NTSC supported by configuration
Addressing: classic 24-bit Amiga CPU address space
Host:       Windows / Linux / macOS
Host CPU:   x86-64 / ARM64
Language:   C++20
Build:      CMake + Ninja
```

The architecture must leave room for:

```text
68010
68EC020 / 68020
AGA / A1200
Fast RAM
2 MiB Chip RAM
CD32-style inputs
```

but these are not blockers for the first usable system.

## 1.2 Explicit non-goals for the MVP

Do not attempt initially to:

- decompile 68k into readable game source;
- automatically understand gameplay structures;
- reconstruct original function or variable names;
- support every Amiga model;
- boot every protected ADF from reset;
- reproduce Kickstart internally;
- support arbitrary self-modifying code transparently;
- guarantee cycle-perfect compatibility with every raster trick;
- rewrite game rendering automatically;
- convert planar art to modern art automatically.

The goal is a reliable recompiler framework, not an instant universal Amiga port generator.

---

# 2. Architectural principle

The project consists of **two independent halves**:

```text
                    BUILD TIME

       game binary / captured memory
                    │
                    ▼
             amiga-recomp tool
                    │
       68k instructions → C++
                    │
                    ▼
              generated code


                    RUN TIME

              generated code
                    │
              AmigaRuntime
                    │
       ┌────────────┼────────────┐
       ▼            ▼            ▼
    memory       chipset      platform
                 runtime       backend
       │            │            │
       │     Copper / Blitter     │
       │     bitplanes / audio    │
       │     CIA / interrupts     │
       │                          │
       └────────────┬─────────────┘
                    ▼
            SDL/host application
```

The **CPU is recompiled**.

The **Amiga hardware is implemented by the runtime**.

This is the same useful split seen in other static recompilation systems: native game logic on one side, machine/runtime compatibility layer on the other.

---

# 3. Repository layout

Create the project with this structure:

```text
amiga-recomp/
│
├── CMakeLists.txt
├── README.md
├── LICENSE
├── docs/
│   ├── architecture.md
│   ├── manifest.md
│   ├── cpu_translation.md
│   ├── hardware_runtime.md
│   ├── tracing.md
│   ├── hooks.md
│   └── porting_game.md
│
├── tools/
│   └── arecomp/
│       ├── CMakeLists.txt
│       └── src/
│           ├── main.cpp
│           ├── config.cpp
│           ├── image.cpp
│           ├── hunk.cpp
│           ├── decoder.cpp
│           ├── effective_address.cpp
│           ├── analyzer.cpp
│           ├── cfg.cpp
│           ├── translator.cpp
│           ├── codegen.cpp
│           ├── trace_import.cpp
│           ├── patch.cpp
│           └── diagnostics.cpp
│
├── include/
│   └── amiga_recomp/
│       ├── cpu_state.hpp
│       ├── guest_memory.hpp
│       ├── runtime.hpp
│       ├── dispatch.hpp
│       ├── exceptions.hpp
│       └── hooks.hpp
│
├── runtime/
│   ├── CMakeLists.txt
│   ├── memory/
│   ├── cpu/
│   ├── chipset/
│   │   ├── custom_regs.cpp
│   │   ├── scheduler.cpp
│   │   ├── copper.cpp
│   │   ├── blitter.cpp
│   │   ├── bitplanes.cpp
│   │   ├── sprites.cpp
│   │   ├── collisions.cpp
│   │   ├── paula.cpp
│   │   ├── disk.cpp
│   │   ├── interrupts.cpp
│   │   └── cia.cpp
│   ├── os/
│   │   ├── hunk_loader.cpp
│   │   └── optional_os_shim.cpp
│   ├── platform/
│   │   ├── sdl_platform.cpp
│   │   ├── input.cpp
│   │   ├── audio.cpp
│   │   └── display.cpp
│   └── debug/
│       ├── trace.cpp
│       ├── reg_overlay.cpp
│       ├── memory_watch.cpp
│       └── hardware_log.cpp
│
├── reference/
│   └── musashi/
│       └── ...
│
├── tests/
│   ├── cpu/
│   ├── addressing/
│   ├── exceptions/
│   ├── memory/
│   ├── copper/
│   ├── blitter/
│   ├── audio/
│   ├── cia/
│   ├── integration/
│   └── differential/
│
├── testroms/
│   ├── synthetic/
│   └── README.md
│
├── examples/
│   ├── copperbars/
│   ├── bitplane/
│   ├── blitter/
│   └── audio/
│
└── generated/
    └── .gitkeep
```

Do not commit commercial game binaries.

---

# 4. Inputs

Support three input modes, implemented in this order.

## 4.1 Mode A — captured-memory image

This is the MVP and the recommended route for the first real game.

Input consists of:

```text
memory.bin
machine state
entry PC
D0-D7
A0-A7
SR
USP/SSP where applicable
vector table
known executable ranges
optional dynamic-target trace
```

This deliberately starts after any difficult loader/Kickstart/protection stage.

It lets the recompiler prove the architecture on real games before trying to reproduce every possible Amiga boot path.

Example:

```toml
[game]
name = "Example Game"
sha256 = "..."
machine = "a500-pal"

[input]
type = "snapshot"
memory = "capture/memory.bin"

[cpu]
pc = 0x00012000
sr = 0x2700
d = [0,0,0,0,0,0,0,0]
a = [0,0,0,0,0,0,0,0x0007ff00]

[[memory.region]]
start = 0x000000
size = 0x080000
kind = "chip"

[[code.range]]
start = 0x00012000
end   = 0x00025fff
```

## 4.2 Mode B — Amiga Hunk executable

Implement next.

Parse at minimum:

```text
HUNK_HEADER
HUNK_CODE
HUNK_DATA
HUNK_BSS
HUNK_RELOC32
HUNK_SYMBOL
HUNK_END
```

Load hunks into a deterministic guest address map.

Apply relocations exactly.

The guest addresses used by the recompilation must be the same addresses exposed to the translated program.

Do not confuse:

```text
file offset
hunk offset
guest runtime address
host pointer
```

They are separate concepts.

Hunk support is useful for OS-friendly games but does not itself provide AmigaOS.

## 4.3 Mode C — ADF / bootblock / trackloader

Implement only after snapshot and Hunk modes are stable.

This mode requires:

- boot block loading;
- disk DMA;
- CIA floppy control;
- track/sector or raw MFM support as required;
- dynamic discovery of newly loaded executable regions;
- overlay/code registration.

A trackloader may load executable code into addresses that did not exist in the initial static image.

The architecture must support this, but do not make it an MVP requirement.

---

# 5. Game manifest

Every port has an explicit manifest.

Use TOML.

Example:

```toml
[game]
name = "Example"
id = "example-pal"
machine = "a500-pal"
sha256 = "012345..."

[input]
type = "snapshot"
path = "capture/memory.bin"

[cpu]
entry = 0x00012000
initial_sr = 0x2700

[video]
standard = "pal"

[recompiler]
strict_address_errors = true
development_interpreter_fallback = true
emit_instruction_comments = true
emit_cycle_accounting = true

[[memory.region]]
start = 0x000000
size = 0x080000
kind = "chip"
permissions = "rwx"

[[memory.region]]
start = 0x00c00000
size = 0x00080000
kind = "slow"
permissions = "rw"

[[code.range]]
start = 0x00012000
end   = 0x00026000

[[indirect_target]]
site = 0x000154ac
targets = [0x00016000, 0x00016124, 0x00016488]

[[patch.instruction]]
address = 0x00018022
replacement = "nop"

[[hook]]
address = 0x0001a820
name = "native_read_controls"
mode = "replace"

[[hook]]
address = 0x0001d100
name = "debug_draw_hook"
mode = "before"
```

The manifest is part of the port, not the generic recompiler.

This is important: **game-specific knowledge is metadata, not reconstructed source**.

---

# 6. CPU state

Use an explicit guest CPU structure.

```cpp
struct M68kState {
    uint32_t d[8]{};
    uint32_t a[8]{};

    uint32_t pc = 0;

    uint16_t sr = 0x2700;

    uint32_t usp = 0;
    uint32_t ssp = 0;

    uint64_t cycles = 0;
};
```

Provide helpers for status bits:

```text
T1/T0 where relevant later
S
interrupt mask I2:I0
X
N
Z
V
C
```

For the 68000 MVP, model:

```text
T
S
I2:I0
X N Z V C
```

Do not use host CPU flags as persistent guest state.

Keep guest state explicit and deterministic.

---

# 7. Guest address space

Use a guest address abstraction.

For the initial classic Amiga target:

```text
guest address width = 24 bits
```

Normalize addresses where correct for the configured CPU/model.

Do not expose raw host pointers to generated game code.

All guest accesses go through:

```cpp
uint8_t  read8 (uint32_t addr);
uint16_t read16(uint32_t addr);
uint32_t read32(uint32_t addr);

void write8 (uint32_t addr, uint8_t value);
void write16(uint32_t addr, uint16_t value);
void write32(uint32_t addr, uint32_t value);
```

Guest memory is big-endian.

The host may be little-endian.

Never use native pointer casts as the guest memory implementation.

## 7.1 Region routing

Memory routing should classify accesses into:

```text
Chip RAM
Fast RAM
slow/Ranger RAM
CIA space
custom-chip registers
ROM or shim
expansion/model-specific ranges
invalid/unmapped
```

The runtime must be configured by machine model.

Important Amiga behaviours from the supplied hardware guide:

- custom hardware is centred on `$DFF000`;
- DMA-visible resources such as Copper lists, bitplanes, sprites and Paula samples live in Chip RAM;
- CIAs occupy the `$BFxxxx` area;
- hardware writes are not ordinary RAM writes;
- `DMACON`, `INTENA`, `INTREQ` and `ADKCON` have SET/CLR semantics.

These must be runtime semantics, not ad-hoc game patches.

---

# 8. Static recompilation unit: basic blocks, not recovered functions

Do **not** require correct source-level function recovery.

Compile **basic blocks keyed by guest PC**.

A generated block has this shape:

```cpp
GuestPc block_00012340(M68kState& cpu, AmigaRuntime& rt)
{
    // translated instruction 0x12340
    ...
    // translated instruction 0x12344
    ...
    return 0x00012352;
}
```

Main execution:

```cpp
while (!rt.quit_requested()) {
    cpu.pc = rt.dispatch(cpu.pc)(cpu, rt);
}
```

This is deliberately simple.

At 7 MHz-class guest speeds, dispatch once per basic block is easily fast enough on modern machines.

Later optimisation may fuse blocks/functions.

Correctness is more important than avoiding one host indirect call per guest basic block.

## 8.1 Block boundaries

End a block on:

```text
BRA
conditional branch
DBcc
BSR
JMP
JSR
RTS
RTE
RTR
TRAP
illegal instruction
privileged exception
STOP
RESET
indirect control flow
explicit configured hook boundary
```

Also split blocks where needed for:

```text
interrupt polling
self-modifying patch sites
debug/watchpoint boundaries
```

## 8.2 Direct control flow

For a direct branch:

```asm
BRA.W $1400
```

generate:

```cpp
return 0x00001400;
```

For:

```asm
BNE.W $1400
```

generate:

```cpp
return !cpu.z() ? 0x00001400 : 0x00001238;
```

## 8.3 JSR / BSR

Preserve the **guest stack**.

Example:

```asm
JSR $18000
```

must conceptually perform:

```cpp
push32(cpu, rt, return_pc);
return 0x00018000;
```

Do not replace it with a normal C++ function call in the MVP.

That would make unusual return-address manipulation harder to preserve.

An optimisation pass may later convert provably conventional direct subroutine calls into host calls.

## 8.4 RTS

Implement:

```cpp
return pop32(cpu, rt);
```

This naturally supports guest return addresses.

## 8.5 JMP (An), JSR (An) and other indirect targets

Generate dynamic dispatch:

```cpp
return cpu.a[n] & rt.address_mask();
```

or for JSR:

```cpp
push32(...);
return target;
```

The dispatcher then resolves the guest address to compiled code.

Unknown target policy:

Development build:

```text
compiled target?
    yes → run compiled block
    no  → log + interpreter fallback
```

Release/native build:

```text
compiled target?
    yes → run
    no  → fatal diagnostic
```

A finished recomp port must be capable of running with the interpreter fallback disabled.

---

# 9. 68000 decoder

Implement a dedicated MC68000 decoder.

Do not put Ghidra on the required runtime/build path.

Ghidra may be supported as an optional analysis/metadata source later.

The decoder must return a structured representation:

```cpp
struct DecodedInstruction {
    uint32_t pc;
    uint16_t opcode;
    Mnemonic mnemonic;
    Operand src;
    Operand dst;
    uint8_t size;
    uint8_t length;
    uint16_t base_cycles;
    uint32_t flags;
};
```

Operand types should represent:

```text
Dn
An
(An)
(An)+
-(An)
d16(An)
d8(An,Xn)
absolute.W
absolute.L
d16(PC)
d8(PC,Xn)
immediate
register list
condition code
branch displacement
```

Do not represent effective-address text and then re-parse strings.

Use typed structures.

---

# 10. Effective-address layer

This is one of the most important components.

Every instruction translator should use common EA helpers.

Example internal interface:

```cpp
struct EA {
    EAKind kind;
    uint8_t reg;
    int32_t displacement;
    uint32_t absolute;
    IndexSpec index;
    uint32_t immediate;
};
```

Generated C++ may call helpers such as:

```cpp
auto src = ea_read16(cpu, rt, EA_...);
ea_write16(cpu, rt, EA_..., value);
```

However, for performance and readability, prefer code generation that specializes known modes.

For:

```asm
MOVE.W (A0)+,D1
```

generate approximately:

```cpp
{
    const uint32_t addr = cpu.a[0];
    const uint16_t value = rt.read16(addr);
    cpu.a[0] += 2;
    cpu.d[1] = (cpu.d[1] & 0xffff0000u) | value;
    set_move_flags_16(cpu, value);
    ADVANCE(8);
}
```

Be careful with A7 byte postincrement/predecrement behaviour.

On 68000, byte accesses involving A7 adjust by 2 to maintain stack alignment.

Implement this centrally and test it explicitly.

---

# 11. Literal translation policy

Generated code should resemble machine semantics.

Good:

```cpp
const uint16_t lhs = lo16(cpu.d[0]);
const uint16_t rhs = rt.read16(cpu.a[1] + 6);
const uint16_t result = add16_with_flags(cpu, lhs, rhs);
cpu.d[0] = replace_lo16(cpu.d[0], result);
```

Bad:

```cpp
player->health += enemy->damage;
```

The latter is decompilation/reverse engineering, not recompilation.

Generated identifiers should use machine-oriented names:

```text
block_001234
loc_001250
guest_00080000
hook_001234
```

Optional user metadata may give friendly labels, but translation must not depend on semantic names.

---

# 12. Instruction semantics

Implement all standard MC68000 instructions before claiming general 68000 support.

Groups include at least:

```text
ABCD
ADD / ADDA / ADDI / ADDQ / ADDX
AND / ANDI
ASL / ASR
BCC / BRA / BSR
BCHG / BCLR / BSET / BTST
CHK
CLR
CMP / CMPA / CMPI / CMPM
DBcc
DIVS / DIVU
EOR / EORI
EXG
EXT
JMP
JSR
LEA
LINK
LSL / LSR
MOVE
MOVEA
MOVE from SR
MOVE to CCR
MOVE to SR
MOVEM
MOVEP
MOVEQ
MULS / MULU
NBCD
NEG / NEGX
NOP
NOT
OR / ORI
PEA
RESET
ROL / ROR
ROXL / ROXR
RTE
RTR
RTS
SBCD
Scc
STOP
SUB / SUBA / SUBI / SUBQ / SUBX
SWAP
TAS
TRAP
TRAPV
TST
UNLK
```

Handle illegal/reserved opcodes via the correct guest exception path.

---

# 13. Flags

Flag correctness is critical.

Implement shared helpers for:

```text
ADD byte/word/long
SUB byte/word/long
CMP
logical operations
shifts
rotates
BCD
MUL/DIV
MOVE
NEG/NEGX
ADDX/SUBX
```

Do not duplicate flag formulas in every translator.

Unit-test edge cases:

```text
0x7f + 1
0xff + 1
0x8000 - 1
0 - 1
signed overflow
carry without overflow
overflow without carry
X preservation
Z accumulation behaviour for ADDX/SUBX/BCD
```

---

# 14. Exceptions and privilege

Implement a real 68000 exception entry path.

At minimum support:

```text
bus error
address error
illegal instruction
divide by zero
CHK
TRAPV
privilege violation
trace
line A
line F
TRAP #0-15
interrupt levels 1-7
```

The stack-frame implementation must follow the configured CPU.

The 68000 has important exception-frame differences from later CPUs.

Do not accidentally implement 68020 exception frames for the 68000 target.

`RTE` must restore the guest state from guest memory.

---

# 15. Interrupt integration

Amiga interrupts are asynchronous.

Translated code must allow the runtime to interrupt execution at instruction boundaries.

Use:

```cpp
#define ADVANCE(cycles) \
    do { \
        rt.advance_cpu_cycles(cpu, cycles); \
        if (rt.interrupt_should_preempt(cpu)) \
            return rt.enter_pending_interrupt(cpu, NEXT_PC); \
    } while (0)
```

The implementation may be inline functions rather than macros.

Every translated instruction accounts guest time.

Do not run an entire frame of native code and then update hardware.

The hardware scheduler and CPU must advance together.

This is essential for:

```text
VERTB
Copper interrupts
blitter interrupts
CIA timers
Paula channel interrupts
beam polling
raster effects
```

---

# 16. Timing model

Implement timing in layers.

## Tier 1 — instruction-cycle timing

Each instruction advances nominal MC68000 cycles.

This is required from the beginning.

## Tier 2 — memory/bus timing

Memory access helpers can add or expose wait-state effects where needed.

## Tier 3 — Chip RAM contention

Later, account for Agnus DMA stealing Chip RAM slots from the CPU.

This becomes important for highly timing-sensitive software.

Do not block the first game on full DMA-slot-perfect timing.

Instead make timing fidelity explicit:

```text
TimingMode::Instruction
TimingMode::BusAware
TimingMode::ChipExact
```

A port manifest may require a minimum timing mode.

---

# 17. Hardware scheduler

All asynchronous hardware uses one monotonically increasing machine time.

Suggested representation:

```cpp
using MasterTick = uint64_t;
```

Keep conversion helpers between:

```text
68000 CPU cycles
colour clocks
scanline position
audio periods
CIA ticks
```

The scheduler should support:

```cpp
MasterTick now() const;
void run_until(MasterTick target);
MasterTick next_event_time() const;
```

Events include:

```text
horizontal beam progression
vertical blank
Copper wakeup
blitter completion / progress
audio DMA fetch / sample event
CIA timer event
disk DMA
interrupt assertion
```

The runtime must never use host wall-clock time to determine guest machine behaviour.

Host time only controls presentation/throttling.

This is required for deterministic tests.

---

# 18. Custom-chip register interface

The runtime exposes custom registers around:

```text
$DFF000
```

Implement semantically rather than as a plain byte array.

At minimum model the important groups:

```text
DMACON / DMACONR
INTENA / INTENAR
INTREQ / INTREQR
ADKCON / ADKCONR

VPOSR / VHPOSR

COP1LC / COP2LC
COPJMP1 / COPJMP2
COPCON

DIWSTRT / DIWSTOP
DDFSTRT / DDFSTOP

BPLxPT
BPLCON0/1/2
BPL1MOD / BPL2MOD

SPRxPT
sprite position/control/data

COLOR00-COLOR31

BLTCON0/1
BLTAFWM/BLTALWM
BLTxPT
BLTxMOD
BLTxDAT
BLTSIZE

AUDxLC/LEN/PER/VOL/DAT
```

Critical rule:

```text
DMACON
INTENA
INTREQ
ADKCON
```

use bit-15 set/clear semantics.

Never model them as:

```cpp
reg = written_value;
```

---

# 19. Copper

Copper execution is part of the hardware runtime, **not recompiled 68k code**.

The runtime fetches Copper instructions from guest Chip RAM.

Implement:

```text
MOVE
WAIT
SKIP
```

with the correct word-pair format.

Support:

```text
COP1LC
COP2LC
COPJMP1
COPJMP2
COPCON restrictions
$FFFF,$FFFE halt pattern
PAL line-255 wrap behaviour
blitter-wait condition
```

A key reason to keep Copper in runtime is that games routinely patch Copper list data dynamically.

The recompiled 68k should simply write to Chip RAM and Copper registers exactly as before.

The runtime then sees the changed list.

This is much better than trying to statically translate Copper lists.

---

# 20. Bitplane/display model

The first renderer should reproduce Amiga output from hardware state.

Do not replace the original renderer yet.

Implement:

```text
OCS planar bitplanes
1-6 planes
LoRes
HiRes where practical
dual playfield
EHB
HAM6
fine scrolling
odd/even modulos
display-window clipping
data-fetch window
palette changes during a scanline where feasible
```

AGA/HAM8 are later.

## 20.1 Render from a register timeline

Do not assume one set of registers per frame.

Copper and CPU may change display registers mid-frame.

Maintain a timestamped hardware timeline or render incrementally as beam time advances.

A scanline renderer is acceptable for the first implementation if it honours the order of register changes that affect that scanline.

This is essential for:

```text
raster bars
split screens
mid-frame bitplane pointer changes
BPLCON1 sine effects
palette gradients
sprite multiplexing
```

## 20.2 Host presentation

Output the resulting logical Amiga framebuffer to SDL.

Support:

```text
nearest-neighbour
integer scaling
aspect correction
overscan crop configuration
full overscan/debug view
VSync optional
```

Do not call scaling to 4K "4K rendering".

It is presentation scaling unless a game-specific replacement renderer exists.

---

# 21. Hardware sprites

Implement eight hardware sprite channels.

Initial requirements:

```text
sprite pointers
POS/CTL
DATA/DATB
DMA fetch
priority against playfields
attached sprites where practical
palette selection
sprite/playfield collision bits
```

Correct collision behaviour may be deferred behind a manifest capability flag for games that do not use it.

---

# 22. Blitter

The blitter is asynchronous.

Do not model a `BLTSIZE` write as an ordinary immediate memory copy with `BBUSY=false`.

Create a `BlitterJob` snapshot when a blit starts.

It records:

```text
BLTCON0/1
AFWM/ALWM
A/B/C/D pointers
A/B/C/D modulos
A/B/C data
width
height
direction
shift
minterm
fill mode
line mode
```

## 22.1 MVP execution

For the first implementation:

- compute an estimated completion time;
- set busy;
- schedule the blit;
- apply the full memory result at completion;
- clear busy;
- assert BLIT interrupt if enabled.

This is not sufficient for code that intentionally observes partially completed blits.

Add a later `IncrementalBlitter` mode that executes in DMA-sized chunks.

## 22.2 Minterm

Implement the 8-input truth table exactly.

Do not hard-code only copy and cookie-cut.

The common cookie-cut `$CA` case must naturally fall out of the general minterm implementation.

## 22.3 Descending/fill/line mode

Implement ordinary ascending area blits first.

Then:

```text
descending
fill
line mode
ECS big-blit extension
```

with test coverage.

---

# 23. Paula audio

Implement four DMA channels.

Each channel tracks:

```text
LC
LEN
PER
VOL
DAT
current DMA address
remaining words
current output byte
period counter
interrupt state
```

Remember:

```text
AUDxLEN counts 16-bit words
```

while output samples are successive bytes.

Use deterministic guest-rate generation into an internal sample stream, then resample/mix to the host audio rate.

Do not let SDL audio callback timing determine Paula timing.

Host callback consumes from a generated ring buffer.

---

# 24. CIA

Implement enough 8520 behaviour for games:

```text
PRA/PRB
DDRA/DDRB
Timer A
Timer B
TOD where needed
SDR
ICR
CRA/CRB
```

Required early use cases:

```text
fire buttons
keyboard serial input
CIA interrupts
timers
floppy control later
```

The CIA runtime belongs to the machine runtime, not generated code.

---

# 25. Input

Expose host input through the original Amiga hardware interface.

Map:

```text
keyboard → Amiga keyboard serial path
controller axes/buttons → joystick port state
mouse → JOYxDAT counters + buttons
```

The original recompiled game should continue reading:

```text
JOY0DAT
JOY1DAT
CIA-A PRA
keyboard events
```

unless a game-specific native hook replaces its input routine.

Provide a user-facing mapping layer separately.

---

# 26. Code discovery

This is a major problem and must be treated explicitly.

Do not assume a linear sweep can distinguish code and data.

Use three sources.

## 26.1 Recursive static discovery

Starting from configured entry points:

```text
entry PC
interrupt vectors
known hook targets
Hunk entry
known overlay entries
```

decode reachable direct control flow.

Follow:

```text
fallthrough
conditional branches
BRA
BSR
direct JSR/JMP
```

Stop on:

```text
RTS/RTE/RTR
indirect JMP/JSR
invalid instruction
configured data boundary
```

## 26.2 Trace-assisted discovery

Support a trace file containing executed PCs and dynamic control-flow edges.

Example:

```text
P 00012000
P 00012004
E 00012008 00018000 JSR_INDIRECT
P 00018000
...
```

Importer should derive:

```text
executed instruction addresses
basic-block starts
indirect targets
runtime-loaded code regions
interrupt handlers reached
```

Trace data is evidence of code execution, not decompiled source.

## 26.3 Explicit manifest metadata

Allow the port author to state:

```toml
[[code.entry]]
address = 0x12340

[[indirect_target]]
site = 0x154ac
targets = [0x16000, 0x16124]
```

Static analysis + trace + explicit metadata form the final compile set.

---

# 27. Reference interpreter

Include a development-only 68000 interpreter.

Preferred initial route:

- use Musashi as a reference implementation;
- wrap its memory callbacks around the exact same `AmigaRuntime` memory and hardware interface.

The interpreter has three jobs:

1. differential correctness testing;
2. trace collection;
3. development fallback for undiscovered target PCs.

It is **not** the production execution engine.

A release port should be able to compile with:

```text
AMIGA_RECOMP_ENABLE_INTERPRETER=OFF
```

and run completely through recompiled code.

---

# 28. Differential testing

This project must be test-driven.

## 28.1 Single-instruction tests

For each supported instruction:

1. randomize CPU state;
2. randomize relevant memory;
3. execute one instruction with the reference CPU;
4. execute generated semantics;
5. compare:

```text
D0-D7
A0-A7
PC
SR
memory writes
exception
nominal cycle count
```

Run thousands of randomized cases.

## 28.2 Basic-block tests

Generate random legal blocks and compare reference vs recompiled execution until block exit.

## 28.3 Game trace differential mode

Run the same snapshot through:

```text
reference interpreter
recompiled blocks
```

Compare after every instruction in a debug mode.

Useful comparison set:

```text
PC
D/A registers
SR
guest writes
custom-register writes
interrupt entry
cycle counter
```

When divergence occurs, print the last 32 guest instructions and hardware events.

This diagnostic is essential.

---

# 29. Generated-code style

For debugging, optionally emit original assembly as comments.

Example:

```cpp
GuestPc block_00012340(M68kState& cpu, AmigaRuntime& rt)
{
    // $00012340: move.w  (a0)+,d1
    {
        const uint32_t addr = cpu.a[0];
        const uint16_t v = rt.read16(addr);
        cpu.a[0] += 2;
        cpu.d[1] = (cpu.d[1] & 0xffff0000u) | v;
        set_move_flags16(cpu, v);
        rt.advance_cpu_cycles(cpu, 8);
        if (rt.interrupt_should_preempt(cpu))
            return rt.enter_pending_interrupt(cpu, 0x00012342);
    }

    // $00012342: addq.w #1,d0
    {
        const uint16_t v = lo16(cpu.d[0]);
        const uint16_t r = add16(cpu, v, 1, false);
        cpu.d[0] = replace_lo16(cpu.d[0], r);
        rt.advance_cpu_cycles(cpu, 4);
        if (rt.interrupt_should_preempt(cpu))
            return rt.enter_pending_interrupt(cpu, 0x00012344);
    }

    return 0x00012344;
}
```

Debug readability matters.

This is not intended to resemble original source.

---

# 30. Dispatcher

Generate a fast guest-address dispatcher.

MVP options:

```text
sorted table + binary search
unordered lookup
two-level page table
perfect/generated switch
```

Prefer a two-level page/block table because guest addresses are dense enough and 24-bit.

For example:

```text
guest page = pc >> 8
slot       = (pc & 0xff) >> 1
```

Each valid instruction/block entry can contain:

```cpp
using BlockFn = GuestPc(*)(M68kState&, AmigaRuntime&);
```

Unknown address invokes diagnostics/fallback.

Do not allocate a gigantic host function-pointer table for every byte unless memory cost is acceptable and measured.

---

# 31. Relocation and overlays

Preserve guest addresses.

For Hunk relocations:

- apply the original relocation to guest memory;
- ensure generated code that embeds relocated constants uses the relocated guest value;
- record code/data segment identity.

For dynamically loaded overlays:

```text
guest address range
binary source
generation/module id
compiled block table
```

The runtime may register/unregister overlay block maps.

If two overlays occupy the same guest address at different times, dispatch must use the currently loaded overlay generation.

Model this explicitly:

```cpp
struct CodeMapping {
    uint32_t guest_start;
    uint32_t guest_end;
    uint32_t generation;
    const BlockTable* table;
};
```

Do not assume one guest address permanently means one function.

---

# 32. Self-modifying code

Static recompilation cannot silently ignore code writes.

Track all writes to pages marked executable.

Development behaviour:

```text
write to executable page
    ↓
log address, old bytes, new bytes, current PC
    ↓
classify
```

Support three outcomes.

## 32.1 Data false-positive

The page was incorrectly marked as code.

Fix metadata.

## 32.2 Known operand patch

Some games patch immediate values or addresses inside instructions.

Allow an annotation that makes the generated translation read the patched field from guest memory at runtime.

Example:

```toml
[[self_modify.operand]]
instruction = 0x001234
field = "immediate16"
```

The translator emits:

```cpp
const uint16_t imm = rt.read16(0x001236);
```

rather than embedding the original constant.

## 32.3 True generated/new code

Development mode may execute it in the interpreter and trace it.

Production static-recomp mode must either:

- have a known finite set of generated forms;
- precompile all variants;
- replace the generator with a native hook;
- or reject the port as requiring dynamic translation.

Do not hide this limitation.

---

# 33. Hardware register logging

Provide an optional semantic logger.

Raw:

```text
W16 DFF096 8380
```

Semantic:

```text
[123456] DMACON set DMAEN|BPLEN|COPEN
```

Raw:

```text
W16 DFF058 0443
```

Semantic:

```text
[123920] BLIT start width=3 words height=17 minterm=CA A=... B=... C=... D=...
```

Copper:

```text
[line 100 h=20] COLOR00 := 00F
```

Audio:

```text
AUD1 start addr=00058c20 words=128 period=428 volume=64
```

These logs make porting practical without turning the project into a decompiler.

---

# 34. Debugger

Provide an integrated debugger suitable for recomp ports.

Commands/features:

```text
break guest address
break custom-register write
watch guest memory
watch executable-page write
step guest instruction
step guest block
run to VBL
run to raster line
show D/A/SR
show Copper state
show blitter state
show bitplane pointers
show active DMA
show interrupt state
dump Chip RAM
dump current Copper list
save trace
```

The debugger operates on **guest addresses**, never generated C++ line numbers as the primary interface.

---

# 35. Hooks and native replacement

Hooks are a central reason to use recompilation.

Support:

```text
before
after
replace
```

Example manifest:

```toml
[[hook]]
address = 0x00018000
name = "native_read_controls"
mode = "replace"
```

Generated block:

```cpp
GuestPc block_00018000(M68kState& cpu, AmigaRuntime& rt)
{
    return hooks::native_read_controls(cpu, rt);
}
```

A replacement hook is responsible for reproducing the observable guest contract:

```text
register outputs
memory writes
stack behaviour
return PC
flags if caller relies on them
timing where relevant
```

Provide helper APIs, not game-specific assumptions.

---

# 36. Native patch modules

A game port may contain:

```text
ports/example/
├── game.toml
├── capture/
├── patches/
│   ├── input.cpp
│   ├── renderer.cpp
│   ├── timing.cpp
│   └── widescreen.cpp
├── assets/
└── CMakeLists.txt
```

The generic framework must not contain knowledge of individual commercial titles.

---

# 37. Modern rendering and widescreen

Do not claim that static recompilation automatically creates high-resolution or widescreen graphics.

There are three enhancement levels.

## Level 0 — faithful Amiga output

Original bitplanes/Copper/sprites rendered to a host texture.

Can be displayed at any host window size but internal game resolution is unchanged.

## Level 1 — hardware-aware enhancement

Examples:

```text
smooth host scaling
aspect correction
overscan control
palette post-processing
CRT shader
higher precision presentation
```

Still based on original Amiga raster.

## Level 2 — game-specific renderer replacement

A port identifies and hooks a game renderer, for example:

```text
polygon submission
line drawer
sprite/BOB renderer
tile renderer
3D projection routine
screen clear
```

Then native code may render at:

```text
1920×1080
2560×1440
3840×2160
ultrawide
```

while the original game logic remains recompiled 68k.

This is where real high-resolution and widescreen remasters come from.

The hook API must make Level 2 possible, but the generic recompiler does not infer it.

---

# 38. OS interaction

Amiga games vary from OS-friendly to complete hardware takeover.

Do not require Kickstart ROM redistribution.

Support three strategies.

## 38.1 Snapshot after takeover

Preferred MVP.

Capture the game after it has loaded and established its own runtime state.

## 38.2 Minimal OS shim

Later implement explicitly configured replacements for common calls such as:

```text
Exec allocation
OpenLibrary
Disable/Enable
Forbid/Permit
graphics WaitTOF
basic DOS Open/Read/Seek/Close
```

Do not try to emulate all AmigaOS accidentally.

## 38.3 User-supplied ROM

Optional future compatibility mode may allow a user-supplied Kickstart ROM, but this starts approaching full-machine emulation and is not the primary recomp workflow.

---

# 39. Snapshot format

Define a simple open capture format.

Directory form:

```text
capture/
├── capture.toml
├── chipram.bin
├── fastram.bin        # optional
├── slowram.bin        # optional
├── cpu.bin or fields in TOML
├── trace.log          # optional
└── hwstate.bin        # optional
```

`capture.toml`:

```toml
[format]
version = 1

[machine]
model = "a500"
video = "pal"
chipset = "ocs"
chip_ram = 524288

[cpu]
model = "68000"
pc = 0x12340
sr = 0x2700
d = [...]
a = [...]
usp = 0
ssp = 0x7ff00

[[region]]
name = "chip"
base = 0
file = "chipram.bin"

[hardware]
# serialised state reference/version if present
state = "hwstate.bin"
```

The format should be easy to generate from future emulator integrations.

---

# 40. Trace format

Use a simple line-oriented or binary versioned format.

Readable v1:

```text
# AMIGA_RECOMP_TRACE 1
I 00012340 3018
I 00012342 5240
B 00012344 00012400 BNE
M W16 00DFF180 00000F80
J 00013000 00020040 JSR_INDIRECT
X 00015000 00015004
```

Record types:

```text
I instruction executed
B branch edge
J indirect control-flow edge
M memory/custom access of interest
X executable-memory write
L code range loaded
V interrupt/vector entry
```

The importer must tolerate duplicate events.

---

# 41. Ghidra integration

Ghidra is optional.

Do not make p-code the required internal IR for the main recomp.

Useful optional import/export features:

```text
import labels
import known function starts
import memory blocks
import code/data ranges
import user comments
import manually resolved indirect targets
```

A Ghidra script may export this to JSON/TOML.

Why not depend on p-code for the core:

- generic p-code adds a large external toolchain dependency;
- the project needs exact 68000 cycle/timing metadata anyway;
- a literal 68000 translator is easier to debug against hardware behaviour;
- generated C++ should remain close to 68k semantics.

Ghidra remains highly valuable for manual reverse engineering of specific games.

---

# 42. Recompiler IR

Use a small machine-oriented IR between decode and C++ emission.

Example:

```cpp
enum class OpKind {
    ReadEA,
    WriteEA,
    AluAdd,
    AluSub,
    LogicAnd,
    LogicOr,
    LogicXor,
    SetFlags,
    Branch,
    Push,
    Pop,
    Exception,
    AdvanceCycles
};
```

However, do not over-engineer SSA or a high-level optimizer.

The IR exists to:

```text
separate decode from codegen
unit-test semantics
support alternate codegen later
enable simple constant specialization
```

It is not a decompiler IR.

---

# 43. Code generation target

Generate portable C++20 first.

Do **not** implement a direct x86-64 JIT backend first.

Benefits:

```text
MSVC/Clang/GCC optimise the output
x86-64 and ARM64 come for free
debuggers work
generated source is inspectable
build system is simple
sanitizers work
LTO is available
```

Potential future targets:

```text
LLVM IR
WASM
Rust
```

but C++ is the reference backend.

---

# 44. Build pipeline

Desired CLI:

```text
arecomp inspect game.toml
arecomp analyze game.toml
arecomp import-trace game.toml trace.log
arecomp generate game.toml -o generated/example
arecomp validate game.toml
```

Generated directory:

```text
generated/example/
├── CMakeLists.txt
├── generated_config.hpp
├── guest_blocks_000.cpp
├── guest_blocks_001.cpp
├── guest_blocks_002.cpp
├── block_table.cpp
├── initial_memory.cpp or external loader metadata
└── build_report.json
```

Avoid one source file per block; that creates excessive build overhead.

Group a configurable number of blocks per translation unit.

Example:

```toml
[codegen]
blocks_per_cpp = 512
```

---

# 45. Build report

Every generation emits a report:

```json
{
  "input_sha256": "...",
  "cpu": "68000",
  "compiled_instruction_count": 12345,
  "compiled_block_count": 1902,
  "direct_edges": 2204,
  "indirect_sites": 18,
  "resolved_indirect_targets": 74,
  "unresolved_indirect_sites": 2,
  "self_modifying_writes_seen": 0,
  "unsupported_opcodes": [],
  "interpreter_fallback_required": true
}
```

A release build should require:

```text
unsupported_opcodes = 0
unresolved runtime target failures = 0
interpreter fallback hits during validation = 0
```

---

# 46. Development fallback

The fallback interpreter is a development accelerator, not a way to fake completion.

When a block is missing:

```text
1. log guest PC;
2. run interpreter;
3. collect executed instructions/targets;
4. add discovered code to trace metadata;
5. regenerate.
```

Provide:

```text
--learn
```

mode that writes a supplemental trace automatically.

Then:

```text
arecomp import-trace ...
arecomp generate ...
```

The intended workflow is iterative convergence toward 100% static coverage.

---

# 47. Determinism

Given:

```text
same input bytes
same initial snapshot
same input event stream
same runtime configuration
```

execution must be deterministic.

Provide deterministic recording/replay of:

```text
keyboard
joystick
mouse
reset
disk insert/eject later
```

This enables regression testing.

---

# 48. Validation artefacts

For every real game port, save test fixtures where legally appropriate:

```text
initial state hash
N-frame final state hash
hardware event hash
frame CRCs/hashes
audio block hashes
input replay
```

Do not store copyrighted full RAM dumps in the public framework repository.

Private/local game-port work may use them.

---

# 49. CPU test strategy

Before running a real game:

## Phase A

Validate:

```text
MOVE
MOVEQ
ADD/SUB/CMP
branches
JSR/RTS
stack
LEA/PEA
MOVEM
shifts/rotates
bit operations
MUL/DIV
exceptions
```

## Phase B

Run standalone synthetic 68000 programs.

## Phase C

Run the same programs under Musashi and the recompiler and compare state.

## Phase D

Run Amiga-specific synthetic binaries that write hardware registers.

Do not debug CPU and Copper simultaneously if CPU correctness has not already been established.

---

# 50. Hardware test programs

Create tiny legal test programs for:

```text
single bitplane
five bitplanes
palette writes
Copper bars
Copper BPLCON1 scroll
sprite
cookie-cut blit
clear blit
blitter interrupt
VERTB interrupt
Paula tone/sample
CIA timer
joystick read
```

These are much easier to diagnose than a commercial game.

---

# 51. Hardware event API

Define a semantic event stream internally:

```cpp
enum class EventType {
    CustomWrite,
    CustomRead,
    CopperMove,
    CopperWait,
    BlitStart,
    BlitComplete,
    AudioDma,
    InterruptAssert,
    InterruptEnter,
    BitplaneFetch,
    FrameStart,
    FrameEnd
};
```

Debug tooling can subscribe without affecting emulation semantics.

This will be useful later for game-specific renderer reverse engineering.

---

# 52. Performance

Do not prematurely optimize CPU translation.

A 7.09 MHz 68000 is tiny relative to modern CPUs.

Priorities:

```text
1. correctness
2. deterministic timing
3. diagnostics
4. ease of porting
5. performance
```

Reasonable optimisations after correctness:

```text
fuse straight-line blocks
inline common EA forms
direct dispatch cache
LTO
omit debug checks in release
specialise RAM accesses known not to hit I/O
lazy flag computation only if proven correct
```

Never optimize by bypassing hardware side effects for an address merely because it usually points to RAM.

---

# 53. Fast direct-memory paths

Generated code may use faster accessors only when address range is statically proven.

Example:

```asm
MOVE.W $00020000,D0
```

may compile to a direct Chip RAM helper.

But:

```asm
MOVE.W (A0),D0
```

must route dynamically unless analysis can prove the range.

Even direct Chip RAM accesses must remain guest-endian and respect alignment rules.

---

# 54. Address errors

On MC68000:

```text
word/long access to odd address
```

raises an address error.

Implement strict mode from the start.

A relaxed debug option may help investigate malformed captures, but ports should run strict.

---

# 55. Supervisor state

Correctly model user/supervisor stack switching.

When `S` changes:

```text
A7 view changes between USP and SSP
```

Centralize this behaviour.

Do not simply store one A7 and ignore USP/SSP.

This matters to exception entry/return and OS-friendly code.

---

# 56. STOP and idle loops

`STOP` halts CPU execution until a suitable interrupt.

Do not busy-loop on the host.

Ask scheduler for the next hardware event capable of waking the CPU.

Similarly, optionally identify common beam-wait loops later, but do not optimize them until correctness is established.

---

# 57. Beam polling

Reads of:

```text
VPOSR
VHPOSR
```

must reflect current scheduler beam position.

A game may spin on these registers.

Instruction-cycle advancement must therefore progress the beam even while the game is busy-waiting.

This is a critical integration test.

---

# 58. DMA visibility

Treat Chip RAM as shared state observed by:

```text
68000
Copper
blitter
bitplane DMA
sprite DMA
Paula audio
disk DMA
```

Do not create unrelated private copies for each subsystem.

One guest Chip RAM backing store must represent shared memory.

Subsystems may cache decoded state, but memory visibility must remain coherent.

---

# 59. Register side effects

Some Amiga registers:

```text
read differently from write
clear on read
trigger an operation on write
have set/clear semantics
```

Represent each register through read/write functions or generated descriptors.

Do not maintain one generic `uint16_t regs[256]` and assume ordinary storage semantics.

A shadow register array is useful for debugging but cannot define behaviour.

---

# 60. Machine profiles

Create data-driven profiles:

```text
a500-pal
a500-ntsc
a600-pal
a1200-pal
```

Initially only `a500-pal` needs to be complete.

Profile fields include:

```text
cpu model
cpu clock
video standard
Chip RAM size
Fast/slow RAM map
chipset generation
CIA configuration
ROM policy
address mask
```

This prevents AGA details from contaminating OCS code paths.

---

# 61. ECS/AGA extension strategy

After OCS/68000 works:

## ECS

Add:

```text
larger Chip RAM variants
SuperHires
programmable timing
DENISEID
BPLCON3 behaviour
big blits
DIWHIGH
```

## AGA

Add:

```text
68EC020/68020 CPU model
32-bit CPU details
2 MiB Chip RAM profile
8 bitplanes
256-entry 24-bit palette
BPLCON4
CLXCON2
FMODE
AGA fetch modes
HAM8
wider sprite fetch
```

Do not fake AGA by merely allowing eight OCS bitplanes.

---

# 62. 68020 extension strategy

Keep CPU model abstraction from the beginning:

```cpp
enum class CpuModel {
    M68000,
    M68010,
    M68EC020,
    M68020
};
```

Decoder and exception model may depend on CPU type.

Do not add 68020 instruction semantics until 68000 differential tests are strong.

---

# 63. Game-port workflow

The intended workflow for a new title is:

```text
1. obtain legal game data;
2. run/capture a stable gameplay snapshot;
3. define memory map and entry state;
4. import a PC/control-flow trace;
5. recursively discover direct code;
6. generate native blocks;
7. run with interpreter fallback;
8. collect missing indirect targets;
9. regenerate;
10. reach zero fallback hits;
11. validate frames/audio/input;
12. add native hooks for enhancements.
```

For a Hunk game:

```text
Hunk load → runtime entry → same process
```

For a trackloader:

```text
boot/disk path or post-loader snapshot → same process
```

---

# 64. What counts as success

The MVP is successful when all of the following are true:

1. A synthetic MC68000 program is statically translated to C++ and runs without an interpreter.
2. Differential CPU tests pass against the reference implementation for the supported instruction set.
3. A Copper-bars test program produces the correct raster output.
4. A bitplane test displays the expected image.
5. A blitter test modifies Chip RAM correctly and generates busy/interrupt state.
6. A Paula test produces deterministic audio.
7. A VERTB-driven test takes interrupts correctly.
8. A captured real A500 game reaches interactive gameplay using recompiled CPU blocks.
9. That game can complete a representative input replay with **zero interpreter fallback hits**.
10. The game runs as a normal host executable using the runtime and user-supplied game data.

---

# 65. What does NOT count as success

Do not call the project a static recomp if the shipping program primarily does:

```cpp
while (running)
    interpret_one_68000_instruction();
```

Do not claim native recompilation merely because the interpreter itself is compiled for x86-64.

Development fallback is allowed.

Production game execution must run the guest instructions through statically generated host code.

Likewise, a source-level rewrite of the game is not the target.

---

# 66. First implementation milestones

## Milestone 0 — skeleton

Implement:

```text
CMake/Ninja
arecomp CLI
manifest parser
runtime skeleton
guest memory
logging
unit-test framework
```

## Milestone 1 — minimal 68k recomp

Support enough to run small programs:

```text
MOVE
MOVEQ
ADDQ/SUBQ
ADD/SUB
CMP
BRA/Bcc
BSR
JSR
JMP
RTS
LEA
TST
CLR
NOP
```

Generated blocks compile and execute.

## Milestone 2 — complete effective addressing

Implement and test every 68000 addressing mode.

## Milestone 3 — full 68000 user instruction set

Add all ordinary MC68000 instructions and randomized differential tests.

## Milestone 4 — exception/interrupt model

Implement:

```text
SR
USP/SSP
vectors
TRAP
illegal
address error
divide-by-zero
interrupts
RTE
STOP
```

## Milestone 5 — Amiga memory/custom registers

Implement:

```text
Chip RAM
CIA decode
custom register decode
DMACON
INTENA/INTREQ
beam state
VERTB
```

## Milestone 6 — video

Implement:

```text
palette
bitplanes
display window
fetch window
modulos
Copper
SDL display
```

## Milestone 7 — blitter

Implement common area blits and async busy/interrupt semantics.

## Milestone 8 — input/audio/CIA

Implement joystick, keyboard path, Paula and CIA timers.

## Milestone 9 — trace-assisted convergence

Implement:

```text
Musashi fallback
trace recorder
trace importer
unknown-target learning
release no-fallback mode
```

## Milestone 10 — real game snapshot

Select one technically simple A500 game with:

```text
68000
OCS
no unusual protection in the captured state
ordinary bitplanes/blitter
minimal self-modifying code
```

Reach gameplay with zero fallback hits.

## Milestone 11 — hooks/mod API

Implement native replacements and demonstrate:

```text
modern controller mapping
debug overlay
one game-specific enhancement
```

---

# 67. Codex implementation rules

Codex must follow these rules while implementing the project.

1. **Do not turn the project into a decompiler.**
2. Keep guest machine state explicit.
3. Generated code must be a literal semantic translation of 68k instructions.
4. Use guest addresses everywhere in recompiled control flow.
5. Hardware accesses always go through the runtime.
6. Every implemented instruction requires tests.
7. Prefer differential tests against the reference CPU.
8. Do not silently approximate flags.
9. Do not silently ignore unsupported instructions.
10. Do not silently ignore odd-address word/long accesses.
11. Do not assume all memory is RAM.
12. Do not treat custom registers like normal memory.
13. Do not treat Copper lists as 68k code.
14. Do not treat blits as necessarily synchronous.
15. Do not make game code depend on host endianness.
16. Do not use host pointers as guest pointers.
17. Do not embed commercial game data in tests or source.
18. Keep interpreter fallback compile-time removable.
19. Keep deterministic guest timing separate from wall-clock presentation.
20. Fail loudly when the runtime reaches an unknown compiled-code address in release mode.

---

# 68. Recommended reference projects and documentation

Use these as architectural references, not code to copy blindly.

## N64Recomp

Concept to borrow:

```text
literal static instruction translation
generated portable C/C++
separate modern runtime
explicit metadata
runtime dispatch for dynamic control flow
patch/hook capability
```

Repository:

```text
https://github.com/N64Recomp/N64Recomp
```

## N64ModernRuntime

Concept to borrow:

```text
keep translated game code separate from host/runtime implementation
```

Repository:

```text
https://github.com/N64Recomp/N64ModernRuntime
```

## Musashi

Use as a development CPU oracle / fallback if licensing remains compatible with the project.

Repository:

```text
https://github.com/kstenerud/Musashi
```

A currently maintained mirror may also be used, but verify provenance/license before vendoring.

## Ghidra SLEIGH / p-code

Use as optional analysis background and potentially for metadata export, not as the mandatory recomp backend:

```text
https://ghidra.re/ghidra_docs/languages/html/sleigh.html
```

## Amiga hardware

The accompanying `AMIGA.md` should be considered the project's practical hardware/reverse-engineering reference.

Where behaviour conflicts with secondary material, verify against the Commodore Amiga Hardware Reference Manual and relevant machine specifications.

---

# 69. First Codex task

Do **not** start by implementing all Amiga hardware.

Start with the static CPU core.

Implement this vertical slice:

```text
input flat binary
      ↓
configured guest load address
      ↓
recursive discovery from entry
      ↓
decode a small MC68000 subset
      ↓
emit C++ basic blocks
      ↓
compile generated project
      ↓
execute against guest memory
      ↓
compare final CPU/RAM state against Musashi
```

Required instruction subset for the first slice:

```text
MOVE.B/W/L
MOVEA
MOVEQ
ADDQ
SUBQ
CMP
TST
LEA
BRA
BEQ
BNE
BSR
JSR
JMP
RTS
NOP
```

Required addressing modes:

```text
Dn
An
(An)
(An)+
-(An)
d16(An)
absolute.W
absolute.L
d16(PC)
immediate
```

Do not add Copper/SDL/audio until this vertical slice has differential tests.

The first demonstration should be a synthetic program that:

```text
loads values
modifies RAM
calls a subroutine
branches based on flags
returns
halts through a test harness sentinel
```

and produces exactly the same guest state under the reference interpreter and generated native code.

---

# 70. Second Codex task

After the first vertical slice is passing:

1. complete all MC68000 effective-address modes;
2. add the remaining instruction families;
3. add SR/exception handling;
4. build randomized single-instruction differential tests;
5. build randomized basic-block differential tests;
6. only then begin the Amiga custom-chip runtime.

---

# 71. Design rationale summary

The project is intentionally **not**:

```text
Amiga binary → pretty C → rewritten game
```

It is:

```text
Amiga binary
    ↓
MC68000 decoder
    ↓
literal machine-semantic IR
    ↓
generated C++ blocks
    ↓
native host compiler
    ↓
AmigaRuntime
    ├── shared guest memory
    ├── Copper
    ├── blitter
    ├── bitplanes/sprites
    ├── Paula
    ├── CIA
    └── interrupts/timing
```

That gives a reusable basis for native Amiga ports without requiring a traditional decompilation.

The decisive architectural choices are:

- **basic-block static recompilation**, not source recovery;
- **guest stack and guest addresses remain real**;
- **literal 68000 state is preserved**;
- **custom chips stay asynchronous hardware programs**;
- **trace-assisted discovery handles real-world indirect control flow**;
- **a reference interpreter is a development oracle, not the shipping engine**;
- **game-specific improvements are hooks layered on top of the faithful recomp**.

Implement correctness and observability first. Modernisation comes after the original game executes deterministically with zero interpreter fallback.
