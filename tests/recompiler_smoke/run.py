#!/usr/bin/env python3
"""Compile and execute synthetic ABI 5 and ABI 6 tests; requires only Python, CMake, C/C++."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

# SHA-256 of the whole generated smoke tree (every file, by relative path) with
# the fast-path emit option off. That output must stay byte-identical to ABI 5;
# update this only for a deliberate ABI 5 emitter change, never for ABI 6 work.
ABI5_GOLDEN = "47c1c658fd35fa91b9a0f92c72268ca8e6f8ed87f1b28a12f58ddaa9936a95ee"

# ABI 6 changes only these files; the block sources must be identical.
ABI6_CHANGED = {"CMakeLists.txt", "recomp_export.c", "recomp_runtime.c", "recomp_runtime.h"}

VARIANTS = (("abi5", None), ("abi6", "1"))


def call(args, expected=0, timeout=120, env=None):
    result = subprocess.run([str(arg) for arg in args], check=False, timeout=timeout, env=env)
    if result.returncode != expected:
        raise RuntimeError(f"exit {result.returncode}, expected {expected}: {args}")


def executable(build, name):
    for candidate in (build / name, build / (name + ".exe"), build / "Release" / (name + ".exe")):
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"missing executable {name} under {build}")


def tree_hash(root):
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        data = path.read_bytes()
        digest.update(path.relative_to(root).as_posix().encode() + b"\0" +
                      str(len(data)).encode() + b"\0" + data)
    return digest.hexdigest()


def check_outputs(abi5, abi6):
    actual = tree_hash(abi5)
    if actual != ABI5_GOLDEN:
        raise RuntimeError(f"ABI 5 output changed: {actual} != {ABI5_GOLDEN}")
    files5 = sorted(p.relative_to(abi5) for p in abi5.rglob("*") if p.is_file())
    files6 = sorted(p.relative_to(abi6) for p in abi6.rglob("*") if p.is_file())
    if files5 != files6:
        raise RuntimeError("ABI 6 export produced a different file set")
    for rel in files5:
        same = (abi5 / rel).read_bytes() == (abi6 / rel).read_bytes()
        if same == (rel.name in ABI6_CHANGED):
            raise RuntimeError(f"unexpected ABI 6 difference state for {rel.as_posix()}")
    for root, abi, fastmem in ((abi5, 5, False), (abi6, 6, True)):
        for module in (root, root / "second"):
            header = (module / "recomp_runtime.h").read_text()
            export = (module / "recomp_export.c").read_text()
            if f"#define RECOMP_IMAGE_ABI {abi}\n" not in header:
                raise RuntimeError(f"{module} is not ABI {abi}")
            if ("recomp_image_fastmem_v1" in export) != fastmem:
                raise RuntimeError(f"{module}: fastmem handshake presence is wrong")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--cc", help="explicit C compiler path, if needed")
    parser.add_argument("--cxx", help="explicit C++ compiler path, if needed")
    args = parser.parse_args()
    source = args.source.resolve()
    test_source = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="suyu-recompiler-smoke-") as tmp:
        root = Path(tmp)
        compiler_options = []
        for key, value in (("C", args.cc), ("CXX", args.cxx)):
            if value:
                compiler_options.append(f"-DCMAKE_{key}_COMPILER={value}")

        def build(name, *options):
            directory = root / name
            call([args.cmake, "-S", test_source, "-B", directory,
                  f"-DSUYU_SOURCE={source}", "-DCMAKE_BUILD_TYPE=Release", *compiler_options,
                  *options])
            call([args.cmake, "--build", directory, "--config", "Release", "--parallel", "2"])
            return directory

        exporter = executable(build("export"), "smoke_export")
        generated = {}
        for name, fastmem in VARIANTS:
            generated[name] = root / f"generated-{name}"
            generated[name].mkdir()
            env = dict(os.environ)
            env.pop("SUYU_RECOMP_AB_FASTMEM", None)
            if fastmem:
                env["SUYU_RECOMP_AB_FASTMEM"] = fastmem
            call([exporter, generated[name]], env=env)
        check_outputs(generated["abi5"], generated["abi6"])

        for name, _ in VARIANTS:
            run = build(f"run-{name}", f"-DGENERATED_DIR={generated[name]}")
            runner = executable(run, "smoke_run")
            for mode in ("slice", "ordinary-page", "cross-page", "special-page",
                         "mem-ordinary", "mem-unmapped", "mem-special", "mem-cross",
                         "mem-unaligned", "mem-limit", "mem-protected"):
                call([runner, mode], timeout=15)
            for mode in ("mutated", "mutated-entry", "unmapped-zero",
                         "cross-page-mutated", "cross-page-unmapped"):
                call([runner, mode], expected=86, timeout=15)
            call([executable(run, "smoke_static")], timeout=15)
            print(f"All synthetic {name.upper()} smoke checks passed.")


if __name__ == "__main__":
    main()
