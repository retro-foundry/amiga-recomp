# Kickstart, Exec and the OS shim

No Kickstart ROM is required, redistributed or emulated. This document
explains why that works for most games, and what to do about the ones where it
does not.

## Most games do not need an operating system

A typical Amiga game boots, disables multitasking, takes over the interrupt
vectors, and talks directly to `$DFF000` from then on. After that takeover
there is no OS call left to service.

That is what makes the snapshot route (AMIGA_RECOMP.md §38.1, and
[decrunching.md](decrunching.md)) so effective: capture memory **after** the
takeover and the OS dependency disappears along with the depacker and the
trackloader. One capture, three problems solved.

If you are working from a snapshot taken after takeover, you can stop reading
here.

## OS-friendly games need a shim

A minority of games — later disk-based titles, some AGA releases, anything
built to run from Workbench — keep calling `exec.library`, `dos.library`,
`graphics.library` and friends throughout.

The approach is a **native shim**, not ROM emulation: implement the library
functions the game actually calls, in C++, behind the same ABI. That is faster
than interpreting ROM code, debuggable, and redistributes nothing.

### How Amiga library calls work

A library call is an indirect jump through a negative offset from the library
base:

```asm
    movea.l _SysBase,a6
    jsr     -198(a6)        ; AllocMem
```

So a shim needs three things:

1. **Library bases** at known guest addresses, with `_SysBase` at `$00000004`
   where `exec.library` puts it.
2. **Jump table stubs** at each negative offset, which the recompiler treats
   as hook sites.
3. **Native implementations** honouring the register argument conventions.

### Generating the table from the NDK

The offsets and register conventions are exactly what the `.fd` function
descriptor files in the Amiga NDK record:

```
##base _SysBase
##bias 30
##public
Supervisor(userFunction)(a5)
...
AllocMem(byteSize,requirements)(d0/d1)
```

Each entry consumes six bytes of vector, starting at `##bias`, including the
`##private` ones. So the LVO of the *n*th entry is `-(bias + 6n)`, and the
argument registers are listed explicitly.

That is machine-readable, so the shim's dispatch table should be **generated**
from a local NDK rather than transcribed by hand.

**The NDK is Commodore/Hyperion copyright and must not be committed to this
repository.** `.gitignore` refuses `*.fd` and the usual NDK directory names.
The generator reads a user-supplied local copy and writes only into
`projects/`, which is itself ignored.

Intended usage:

```sh
arecomp osshim --ndk "D:/amiga-dev/NDK/NDK_3.1/INCLUDES&LIBS/FD" \
               --libraries exec,dos,graphics \
               -o projects/<id>/patch/osshim
```

Status: **not yet implemented.** The format is understood and the hook
machinery it needs already exists; it is scheduled behind getting a real game
running from a snapshot, because that path needs no shim at all.

### Implementing only what is called

Do not implement a library. Implement the functions a specific game calls,
which is usually a dozen or two: `AllocMem`, `FreeMem`, `Forbid`, `Permit`,
`Disable`, `Enable`, `OpenLibrary`, `CloseLibrary`, `DoIO`, `WaitPort`,
`FindTask`, `SetIntVector`, and a handful of `dos.library` file calls.

Anything else should **fail loudly**. A shim that silently returns zero from
an unimplemented call produces a game that misbehaves in a way nobody traces
back to the shim for days.

## Third option: user-supplied ROM

A user who legally owns a Kickstart ROM can supply it, and it can be mapped
read-only at `$F80000` and executed as ordinary recompiled or interpreted 68k
code. The framework supports mapping it (`GuestMemory::add_rom`), but nothing
about the project depends on it and no ROM is distributed.
