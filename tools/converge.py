#!/usr/bin/env python3
"""Feed a port's run log back into its manifest.

The recompiler discovers code by following control flow it can see. Computed
jumps, jump tables and interrupt vectors it cannot, so a first translation
always leaves gaps. Running the port reports every address the dispatcher
reached with no compiled block; adding those to the manifest and regenerating
closes the gaps. Repeat until the report is empty.

This is the convergence loop from AMIGA_RECOMP.md 26.2, and it is the normal
way a port is brought up.

    arecomp recompile ports/game.toml -o projects/game
    ... build and run, capturing stderr to run.log ...
    python tools/converge.py ports/game.toml run.log --snapshot mem.bin
    arecomp recompile ports/game.toml -o projects/game    # and again

Addresses in memory that is entirely zero in the snapshot are reported but not
added: that is code the game loads at run time, and translating the snapshot's
zeros would produce a port that runs into nothing. Those regions belong to the
interpreter fallback, or to a later capture (AMIGA_RECOMP.md 31).
"""

import argparse
import re
import sys

TARGET = re.compile(r"no compiled block at ([0-9a-fA-F]+)")
ENTRY_BLOCK = re.compile(r"entry_points\s*=\s*\[(.*?)\]", re.S)
ADDRESS = re.compile(r"0x([0-9a-fA-F]+)")


def read_targets(path):
    targets = set()
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = TARGET.search(line)
            if match:
                targets.add(int(match.group(1), 16))
    return targets


def existing_entry_points(text):
    match = ENTRY_BLOCK.search(text)
    if not match:
        return set(), match
    return {int(a, 16) for a in ADDRESS.findall(match.group(1))}, match


def looks_like_code(snapshot, address, window=16):
    """False when the snapshot holds nothing at this address.

    A game that loads code from disk leaves those regions empty at capture
    time. Translating them would compile zeros.
    """
    if snapshot is None:
        return True
    if address + window > len(snapshot):
        return False
    return any(snapshot[address:address + window])


def format_entry_points(addresses, per_line=6):
    lines = []
    ordered = sorted(addresses)
    for i in range(0, len(ordered), per_line):
        chunk = ", ".join(f"0x{a:08x}" for a in ordered[i:i + per_line])
        lines.append("  " + chunk + ",")
    return "entry_points = [\n" + "\n".join(lines).rstrip(",") + "\n]"


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifest")
    parser.add_argument("log", help="stderr from a run of the port")
    parser.add_argument("--snapshot",
                        help="the snapshot the port was built from; used to "
                             "reject addresses that hold nothing")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    snapshot = None
    if args.snapshot:
        with open(args.snapshot, "rb") as handle:
            snapshot = handle.read()

    targets = read_targets(args.log)
    if not targets:
        print("no unresolved targets in the log: this port has converged")
        return 0

    text = open(args.manifest, encoding="utf-8").read()
    known, match = existing_entry_points(text)

    fresh = {a for a in targets if a not in known}
    usable = {a for a in fresh if looks_like_code(snapshot, a)}
    empty = fresh - usable

    print(f"log reports        : {len(targets)} distinct unresolved targets")
    print(f"already recorded   : {len(targets & known)}")
    print(f"new and translatable: {len(usable)}")
    if empty:
        low, high = min(empty), max(empty)
        print(f"new but empty      : {len(empty)}  (${low:06x}..${high:06x})")
        print("  These hold nothing in the snapshot, so the game loads them at")
        print("  run time. Left to the interpreter fallback; see docs/decrunching.md.")

    if not usable:
        print("\nnothing to add")
        return 0

    merged = known | usable
    block = format_entry_points(merged)

    if match:
        text = text[:match.start()] + block + text[match.end():]
    else:
        text = text.rstrip() + (
            "\n\n# Entry points observed in a trace run: addresses execution\n"
            "# reached that static discovery could not predict. Metadata, not\n"
            "# inference (AMIGA_RECOMP.md 26.2, 26.3).\n[code]\n" + block + "\n")

    if args.dry_run:
        print("\n(dry run: manifest not written)")
        return 0

    open(args.manifest, "w", encoding="utf-8").write(text)
    print(f"\n{args.manifest}: now records {len(merged)} entry points")
    print("Regenerate the port and run it again; repeat until this reports none.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
