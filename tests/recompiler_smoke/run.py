#!/usr/bin/env python3
"""Compile and execute synthetic ABI5 tests; requires only Python, CMake, C/C++."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def call(args, expected=0, timeout=120):
    result = subprocess.run([str(arg) for arg in args], check=False, timeout=timeout)
    if result.returncode != expected:
        raise RuntimeError(f"exit {result.returncode}, expected {expected}: {args}")


def executable(build, name):
    for candidate in (build / name, build / (name + ".exe"), build / "Release" / (name + ".exe")):
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"missing executable {name} under {build}")


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
        generated = root / "generated"
        generated.mkdir()
        compiler_options = []
        for key, value in (("C", args.cc), ("CXX", args.cxx)):
            if value:
                compiler_options.append(f"-DCMAKE_{key}_COMPILER={value}")
        for stage in ("export", "run"):
            build = root / stage
            options = [f"-DGENERATED_DIR={generated}"] if stage == "run" else []
            call([args.cmake, "-S", test_source, "-B", build,
                  f"-DSUYU_SOURCE={source}", "-DCMAKE_BUILD_TYPE=Release", *compiler_options, *options])
            call([args.cmake, "--build", build, "--config", "Release", "--parallel", "2"])
            if stage == "export":
                call([executable(build, "smoke_export"), generated])
        runner = executable(root / "run", "smoke_run")
        for mode in ("slice", "ordinary-page", "cross-page", "special-page"):
            call([runner, mode], timeout=15)
        for mode in ("mutated", "mutated-entry", "unmapped-zero"):
            call([runner, mode], expected=86, timeout=15)
        call([executable(root / "run", "smoke_static")], timeout=15)
        print("All synthetic ABI5 smoke checks passed.")


if __name__ == "__main__":
    main()
