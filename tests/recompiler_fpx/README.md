# FP differential harness

Run from the repository root:

```sh
python tests/recompiler_fpx/run.py [--jobs N] [--cases N] [--legs 1234] [--golden FILE] [--controls]
```

Requires Python 3, CMake 3.20+ and a C/C++20 compiler (MSVC, clang-cl, GCC or
Clang). `--cc`, `--cxx`, `--generator` and `--cflags` pick the toolchain and the
flags of the generated code, for the compiler matrix.

`fpx_gen` translates every instruction word in `words.h` (generated from
`ops.txt` by `mkwords.py`) with the current emitter into `ops_soft.c`, the
default ABI 5 text. On an AArch64 host it also writes `hw.c`, which executes the
same words natively. `fpx_driver` then runs identical inputs through each
implementation and compares the whole of q0, x0, NZCV and the final guest FPSR,
for seven FPCR settings (0, FZ, DN, RP, RM, RZ, FZ|DN) and start FPSRs with and
without IXC.

Legs:

1. L1, random: edge, raw, moderate, near-underflow, near-overflow, short-mantissa
   and near-integer operands, integer sources, forced cancellation.
2. L2, special grid: all pairs (and triples for three-operand forms) of 64
   special values per width, and 64 special integers.
3. L3, adversarial: products that put a*b+z within 2^-60 of a binary32
   midpoint, exact ties, results around the smallest normal and FLT_MAX+ulp/2,
   subnormal inputs with normal results, inf*0 and tiny exact sums.
4. L4, exhaustive unary: every 2^32 input (`--l4-step 1`) of FSQRT S, FCVT S->D,
   FCVTZS/FCVTZU S->W and SCVTF/UCVTF W->S at FPCR 0 and FZ.

L5 closes the chain to hardware. On an AArch64 host, `--write-golden FILE`
records one FNV-1a hash of the hardware results per leg (1, 2, 4), word and
FPCR; any other host then checks its own results against that file with
`--golden FILE`. Inputs are seeded per word, FPCR and leg, so the file does not
depend on which words or shards are run, and the `--cases`/`--l4-step` values
must match the ones the file was written with.

On an AArch64 host `diff` compares the emitted code against the hardware; on
other hosts, where soft is the only implementation, run L5.
