# Static recompiler smoke tests

Run from the repository root:

```sh
python tests/recompiler_smoke/run.py
```

Requires Python 3, CMake 3.20+, and a C/C++20 compiler (MSVC or GCC).
The runner uses CMake's normal compiler discovery; `--cc`, `--cxx`, and
`--cmake` accept explicit tool paths. `--source` can select another suyu tree.
All generated files and binaries live in a temporary directory.

The suite exports only synthetic arithmetic and branch instructions using the
current emitter and runtime. It checks ABI 4, exact slice budgets of 1/3/4096,
bounded stack use, SVC and breakpoint stops, interior entries, BLR through x30,
and ordinary, cross-page, special, mutated, and unmapped code guards. Expected
guard aborts run in separate processes and must identify the rejected PC before
any guest effect. Each runtime case has a 15-second timeout.

A second executable links two generated static libraries using their emitted
CMake projects and one shared runtime. It checks that lookup indexes, load bases,
guard negotiation, and execution remain independent, including after rebasing
one module. Missing per-module symbol aliases therefore fail at the actual link.

The block-count assertions also exercise the host accounting formula: the first
block is counted on entry; a budget decrement to zero parks the next PC without
executing another block. This suite checks generated-code behavior, not full
emulator integration or title compatibility.
