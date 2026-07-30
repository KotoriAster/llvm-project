#!/usr/bin/env python3
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# ==------------------------------------------------------------------------==#

"""Benchmark the experimental Mach-O input-loading schedulers.

The script extracts an existing executable's final link command from Ninja,
redirects the compiler driver to the supplied ld64.lld, and interleaves serial,
ios, elf, and prime links to reduce time-order bias.

Example:
  lld/utils/benchmark_macho_input_loading.py \
    --build-dir build-lld-parallelization \
    --target bin/lld \
    --linker build-lld-parallelization/bin/ld64.lld \
    --workers 18 --runs 10 --output lld-input-loading.json
"""

import argparse
import hashlib
import json
import os
import random
import resource
import shlex
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


MODES = ("serial", "ios", "elf", "prime")


def extract_link_command(build_dir: Path, target: str) -> list[str]:
    output = subprocess.check_output(
        ["ninja", "-C", str(build_dir), "-t", "commands", target],
        text=True,
    )
    link_command = None
    for line in output.splitlines():
        for command in line.split("&&"):
            if " -o " in command:
                link_command = command.strip()
    if link_command is None:
        raise RuntimeError(f"could not find the link command for {target}")
    return shlex.split(link_command)


def replace_output(command: list[str], output: Path) -> list[str]:
    result = list(command)
    output_indices = [i for i, arg in enumerate(result[:-1]) if arg == "-o"]
    if not output_indices:
        raise RuntimeError("link command has no separate '-o <path>' argument")
    result[output_indices[-1] + 1] = str(output)
    return result


def configure_linker(
    command: list[str], linker: Path, mode: str, workers: int
) -> list[str]:
    result = [
        arg
        for arg in command
        if not arg.startswith("-fuse-ld=")
        and not arg.startswith("-Wl,--input-load-demo=")
        and not arg.startswith("-Wl,--input-load-workers=")
    ]

    executable = Path(result[0]).name
    direct_lld = executable in {"ld64.lld", "lld"}
    if direct_lld:
        result[0] = str(linker)
        if mode != "serial":
            result.extend(
                [
                    f"--input-load-demo={mode}",
                    f"--input-load-workers={workers}",
                ]
            )
    else:
        result.insert(1, f"-fuse-ld={linker}")
        if mode != "serial":
            result.extend(
                [
                    f"-Wl,--input-load-demo={mode}",
                    f"-Wl,--input-load-workers={workers}",
                ]
            )
    return result


def run_link(command: list[str], cwd: Path) -> dict[str, float]:
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter()
    proc = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    wall = time.perf_counter() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if proc.returncode != 0:
        rendered = shlex.join(command)
        raise RuntimeError(
            f"link failed with exit code {proc.returncode}\n"
            f"command: {rendered}\n{proc.stderr}"
        )
    return {
        "wall": wall,
        "user": after.ru_utime - before.ru_utime,
        "system": after.ru_stime - before.ru_stime,
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def summarize(samples: list[dict[str, float]]) -> dict[str, float]:
    walls = [sample["wall"] for sample in samples]
    return {
        "count": len(walls),
        "median": statistics.median(walls),
        "mean": statistics.mean(walls),
        "stdev": statistics.stdev(walls) if len(walls) > 1 else 0.0,
        "min": min(walls),
        "max": max(walls),
    }


def print_results(results: dict[str, list[dict[str, float]]]) -> None:
    summaries = {mode: summarize(samples) for mode, samples in results.items()}
    serial = summaries["serial"]["median"]
    print(
        f"{'mode':<8} {'runs':>5} {'median(s)':>10} {'mean(s)':>10} "
        f"{'stdev':>10} {'speedup':>9}"
    )
    for mode in MODES:
        summary = summaries[mode]
        speedup = serial / summary["median"]
        print(
            f"{mode:<8} {summary['count']:>5} {summary['median']:>10.4f} "
            f"{summary['mean']:>10.4f} {summary['stdev']:>10.4f} "
            f"{speedup:>8.3f}x"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--target", required=True)
    parser.add_argument("--linker", required=True, type=Path)
    parser.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--work-dir",
        type=Path,
        help="Directory for temporary linked executables (default: system temp)",
    )
    parser.add_argument(
        "--keep-outputs",
        action="store_true",
        help="Keep the last executable produced by each mode",
    )
    parser.add_argument(
        "--skip-output-check",
        action="store_true",
        help="Do not verify that all four modes produce byte-identical output",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.workers <= 0:
        raise ValueError("--workers must be positive")
    if args.runs <= 0 or args.warmups < 0:
        raise ValueError("--runs must be positive and --warmups non-negative")

    build_dir = args.build_dir.resolve()
    # Preserve an ld64.lld symlink instead of resolving it to the generic
    # multi-call "lld" binary; the basename selects the Mach-O driver flavor.
    linker = Path(os.path.abspath(args.linker))
    if not linker.is_file():
        raise FileNotFoundError(linker)

    source_command = extract_link_command(build_dir, args.target)
    if args.work_dir:
        work_dir = args.work_dir.resolve()
        work_dir.mkdir(parents=True, exist_ok=True)
        remove_work_dir = False
    else:
        work_dir = Path(tempfile.mkdtemp(prefix="lld-input-loading-"))
        remove_work_dir = True

    commands = {}
    outputs = {}
    for mode in MODES:
        output = work_dir / f"{Path(args.target).name}-{mode}"
        command = replace_output(source_command, output)
        commands[mode] = configure_linker(command, linker, mode, args.workers)
        outputs[mode] = output

    try:
        output_hashes = {}
        if not args.skip_output_check:
            verify_output = work_dir / f"{Path(args.target).name}-verify"
            for mode in MODES:
                verify_output.unlink(missing_ok=True)
                verify_command = replace_output(source_command, verify_output)
                verify_command = configure_linker(
                    verify_command, linker, mode, args.workers
                )
                run_link(verify_command, build_dir)
                output_hashes[mode] = sha256(verify_output)
            verify_output.unlink(missing_ok=True)
            if len(set(output_hashes.values())) != 1:
                details = "\n".join(
                    f"  {mode}: {digest}"
                    for mode, digest in output_hashes.items()
                )
                raise RuntimeError(
                    "input-loading modes produced different executables:\n"
                    + details
                )
            print(
                "output check: all modes produced "
                + next(iter(output_hashes.values()))
            )

        for _ in range(args.warmups):
            for mode in MODES:
                outputs[mode].unlink(missing_ok=True)
                run_link(commands[mode], build_dir)

        results = {mode: [] for mode in MODES}
        rng = random.Random(0)
        for iteration in range(args.runs):
            order = list(MODES)
            rng.shuffle(order)
            for mode in order:
                outputs[mode].unlink(missing_ok=True)
                sample = run_link(commands[mode], build_dir)
                sample["iteration"] = iteration
                results[mode].append(sample)

        print_results(results)
        document = {
            "build_dir": str(build_dir),
            "target": args.target,
            "linker": str(linker),
            "workers": args.workers,
            "runs": args.runs,
            "warmups": args.warmups,
            "output_sha256": output_hashes,
            "summaries": {
                mode: summarize(samples) for mode, samples in results.items()
            },
            "samples": results,
        }
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(document, indent=2) + "\n")
            print(f"wrote {args.output}")
    finally:
        if not args.keep_outputs:
            for output in outputs.values():
                output.unlink(missing_ok=True)
        if remove_work_dir:
            try:
                work_dir.rmdir()
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
