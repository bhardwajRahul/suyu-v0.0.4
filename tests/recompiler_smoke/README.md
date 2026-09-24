# Static recompiler smoke tests

Run from the repository root:

```sh
python tests/recompiler_smoke/run.py
```

Requires Python 3, CMake 3.20+, and a C/C++20 compiler (MSVC or GCC).
The runner uses CMake's normal compiler discovery; `--cc`, `--cxx`, and
`--cmake` accept explicit tool paths. `--source` can select another suyu tree.
All generated files and binaries live in a temporary directory.

The suite exports only synthetic arithmetic, branch and memory instructions
using the current emitter and runtime: as ABI 5, as ABI 6 with the FM1
page-table fast path (`SUYU_RECOMP_AB_FASTMEM=1`), and as ABI 6 with FM1 and
the generation code guard. The ABI 5 and ABI 6 outputs must each match a golden
SHA-256 of the whole generated tree, so any change to them is deliberate; the
ABI 6 output may differ from ABI 5 only in the runtime, export and CMake files.
Every runtime case below runs against all three builds.

It checks the image ABI, exact slice budgets of 1/3/4096,
bounded stack use, SVC and breakpoint stops, interior entries, BLR through x30,
and ordinary, cross-page, special, mutated, and unmapped code guards. Expected
guard aborts run in separate processes and must identify the rejected PC before
any guest effect. Each runtime case has a 15-second timeout.

The `mem-*` modes run one block of loads and stores (every width, pairs, Q,
LDAR/STLR, LDXR/STXR) against a table shaped like `Common::PageTable`, with a
logging host callback. Ordinary, unmapped, GPU-tracked and debug pages, page
crossings into non-adjacent host pages, unaligned accesses, a non-page-aligned
address limit, high and tagged address bits, and a guest read-only page are
covered. Each case is checked against a byte model, and on ABI 6 is run with
the fast path on and off, which must give the same registers, memory and
callback sequence. `mem-ordinary` also proves that the fast path, not the ABI
5 walk, serves ordinary accesses.

For ABI 6, `smoke_features` checks the host's feature refusal
(`core/arm/recomp/recomp_image_features.h`) against the image's
`recomp_image_features()`: this host accepts exactly the bits its emitter
produces, and a host lacking any of them refuses the image.

A third export, `abi6gg`, adds the generation code guard (GG1,
`SUYU_RECOMP_AB_GUARD_GEN=1`). It may differ from ABI 6 only in the block units
and the four top-level files, and every mode above runs against it as well,
with the generation word left at "verify always", which must behave exactly as
ABI 5. `smoke_gg_host` then drives the generated module with the real host
manager (`core/arm/recomp/recomp_guard_gen.cpp`):

- the handshake and skip-after-verify protocol;
- for every hook that must move the generation (map, unmap, protect, alias in
  another table, device map, IC IVAU, whole-cache invalidation, new table, new
  process, rebase): verify, change the code, fire the hook, and require the
  next entry to abort (exit 86);
- activation, decided from what the hooks recorded since the table was
  created, that must pin "verify always" (an alias existing before the first
  run, writable or partly unmapped code, a table created before registration,
  an overflowing log), and controls that must not bump;
- `race-bump`, three verifying threads against a thread bumping 200,000 times,
  and `race-mutate-N`, a thread that changes the code, bumps and publishes while
  another enters the block, at 50 different delays.

Pass `-DSMOKE_SANITIZE=thread` to build `smoke_gg_host` under ThreadSanitizer.

A second executable links two generated static libraries using their emitted
CMake projects and one shared runtime. It checks that lookup indexes, load bases,
guard negotiation, and execution remain independent, including after rebasing
one module. Missing per-module symbol aliases therefore fail at the actual link.

The block-count assertions also exercise the host accounting formula: the first
block is counted on entry; a budget decrement to zero parks the next PC without
executing another block. This suite checks generated-code behavior, not full
emulator integration or title compatibility.
