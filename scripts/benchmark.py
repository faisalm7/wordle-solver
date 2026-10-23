#!/usr/bin/env python3
"""Measure an existing Wordle binary; never rebuild or use persistent caches.

Example:
  python3 scripts/benchmark.py --output benchmarks/results.json \
      --build-flags='-O3 -DNDEBUG -std=c++20 -pthread'
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]


def positive_int(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def optional_command(command: list[str]) -> str | None:
    try:
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True,
                                check=False, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def summary(samples: list[float]) -> dict[str, object]:
    ordered = sorted(samples)
    # Nearest-rank percentile: ceil(0.95 * N), using a one-based rank.
    return {
        "samples": len(samples),
        "median_ms": statistics.median(samples),
        "p95_ms": ordered[math.ceil(0.95 * len(samples)) - 1],
        "min_ms": ordered[0],
        "max_ms": ordered[-1],
        "samples_ms": samples,
    }


def hardware() -> dict[str, object]:
    result: dict[str, object] = {
        "os": platform.platform(),
        "architecture": platform.machine(),
        "logical_cpus": os.cpu_count(),
        "python": platform.python_version(),
    }
    if platform.system() == "Darwin":
        result["cpu"] = optional_command(["sysctl", "-n", "machdep.cpu.brand_string"])
        result["model"] = optional_command(["sysctl", "-n", "hw.model"])
        memory = optional_command(["sysctl", "-n", "hw.memsize"])
        result["memory_bytes"] = int(memory) if memory and memory.isdecimal() else None
    else:
        result["cpu"] = platform.processor() or None
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
        epilog="Runs full historical-corpus evaluations. Means named 'solved' exclude failures. "
               "A fresh process is not a flushed operating-system filesystem cache.")
    parser.add_argument("--binary", type=Path, default=ROOT / "build/wordle")
    parser.add_argument("--iterations", type=positive_int, default=21,
                        help="native ranking samples per state size (default: 21)")
    parser.add_argument("--startup-samples", type=positive_int, default=31,
                        help="independent optimal-book CLI processes (default: 31)")
    parser.add_argument("--timeout-seconds", type=positive_int, default=600,
                        help="timeout for each native command (default: 600)")
    parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"),
                        help="compiler command for version metadata; no compilation is performed")
    parser.add_argument("--build-flags", default=os.environ.get("CXXFLAGS"),
                        help="actual flags used for the existing binary; record explicitly for published results")
    parser.add_argument("--output", type=Path,
                        help="save JSON here; otherwise print JSON to stdout")
    parser.add_argument("--skip-dynamic-evaluation", action="store_true",
                        help="omit the full dynamic evaluation; other measurements still run")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    binary = args.binary.expanduser().resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f"executable not found: {binary}; build it before benchmarking")
    try:
        binary_label = "./" + str(binary.relative_to(ROOT))
    except ValueError:
        binary_label = str(binary)

    def run(arguments: list[str], *, evaluation: bool = False,
            display_arguments: list[str] | None = None) -> dict[str, object]:
        start = time.perf_counter_ns()
        completed = subprocess.run([str(binary), *arguments], cwd=ROOT, text=True,
                                   capture_output=True, timeout=args.timeout_seconds, check=False)
        wall_ms = (time.perf_counter_ns() - start) / 1_000_000
        allowed = (0, 2) if evaluation else (0,)
        if completed.returncode not in allowed:
            raise RuntimeError(f"{binary_label} {' '.join(arguments)} failed "
                               f"({completed.returncode}): {completed.stderr.strip()}")
        try:
            payload = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError("native command returned invalid JSON: " + completed.stdout[:200]) from error
        if not isinstance(payload, dict):
            raise RuntimeError("native command returned a non-object JSON result")
        record: dict[str, object] = {
            "command": [binary_label, *(display_arguments or arguments)],
            "wall_ms": wall_ms,
            "exit_code": completed.returncode,
            "result": payload,
        }
        if completed.stderr.strip():
            record["stderr"] = completed.stderr.strip()
        if evaluation:
            record["mean_guesses_all_answers"] = (
                payload.get("mean_guesses_solved") if payload.get("failed") == 0 else None)
        return record

    def progress(message: str) -> None:
        print(message, file=sys.stderr, flush=True)

    dictionary = {}
    for name in ("answers", "guesses"):
        path = ROOT / "data" / f"{name}.txt"
        dictionary[name] = {"words": len(path.read_text(encoding="ascii").splitlines()),
                            "sha256": digest(path)}

    compiler_command = shlex.split(args.compiler)
    source_status = optional_command(["git", "status", "--porcelain"])
    result: dict[str, object] = {
        "schema_version": 1,
        "captured_utc": datetime.now(timezone.utc).isoformat(),
        "environment": hardware(),
        "build": {
            "binary": binary_label,
            "binary_sha256": digest(binary),
            "binary_bytes": binary.stat().st_size,
            "version": optional_command([str(binary), "--version"]),
            "compiler_command": args.compiler,
            "compiler_version": optional_command([*compiler_command, "--version"]) if compiler_command else None,
            "build_flags_declared": args.build_flags,
            "build_flags_note": "Supplied metadata; this driver does not rebuild or infer binary compiler flags.",
            "source_commit": optional_command(["git", "rev-parse", "HEAD"]),
            "source_dirty": None if source_status is None else bool(source_status),
        },
        "dictionary": {"corpus": "original historical Wordle", **dictionary},
        "methodology": {
            "clock": "Python perf_counter_ns for process wall time; native steady_clock for internal times",
            "percentile": "nearest rank: sorted_samples[ceil(0.95 * N) - 1]",
            "startup": "Each sample starts a fresh process with --no-cache; operating-system file caches are uncontrolled.",
            "internal_book_timing": "ranking_ms includes recommendation lookup and feedback statistics, and excludes JSON output and process startup.",
            "ranking": "Native rank scans all allowed guesses; not a memoized book lookup. Each state size has an untimed warmup.",
            "cache": "Cold and warm runs share a new temporary matrix cache, deleted after measurement.",
            "quality": "Uniform distribution over all 2,315 historical answers, six-turn limit. Solved-only means must be read with the failure count.",
            "comparison_limit": "These measurements do not establish a world speed record or compare other solvers.",
        },
    }

    progress(f"Measuring {args.startup_samples} fresh optimal-book CLI processes...")
    samples: list[float] = []
    internal_samples: list[float] = []
    first_result = None
    for _ in range(args.startup_samples):
        record = run(["suggest", "--no-cache", "--json"])
        payload = record["result"]
        if payload.get("policy") != "optimal-book":
            raise RuntimeError("default suggestion did not use the optimal book")
        if payload.get("matrix_ms") != 0:
            raise RuntimeError("optimal-book suggestion unexpectedly built a feedback matrix")
        samples.append(record["wall_ms"])
        internal_samples.append(payload["ranking_ms"])
        if first_result is None:
            first_result = {key: payload[key] for key in
                            ("candidate_count", "policy", "recommendation", "matrix_ms", "cache_loaded")}
    book: dict[str, object] = {
        "fresh_process_suggest": {
            "command": [binary_label, "suggest", "--no-cache", "--json"],
            "process_wall": summary(samples),
            "internal_recommendation": summary(internal_samples),
            "first_result": first_result,
        }
    }
    progress("Evaluating the optimal book on all historical answers...")
    book["evaluation"] = run(["evaluate", "--no-cache", "--json"], evaluation=True)
    result["optimal_book"] = book

    native_options = ["--dynamic", "--iterations", str(args.iterations), "--json"]
    dynamic: dict[str, object] = {}
    progress(f"Measuring {args.startup_samples} fresh off-book history processes...")
    samples = []
    internal_samples = []
    first_result = None
    history_command = ["suggest", "--history", "crane:01002", "--dynamic", "--no-cache", "--json"]
    for _ in range(args.startup_samples):
        record = run(history_command)
        payload = record["result"]
        if payload.get("candidate_count") != 40 or payload.get("matrix_ms") != 0:
            raise RuntimeError("small off-book state unexpectedly built a full matrix or changed corpus")
        samples.append(record["wall_ms"])
        internal_samples.append(payload["ranking_ms"])
        if first_result is None:
            first_result = {key: payload[key] for key in
                            ("candidate_count", "policy", "recommendation", "matrix_ms", "cache_loaded")}
    dynamic["fresh_process_history"] = {
        "command": [binary_label, *history_command],
        "process_wall": summary(samples),
        "internal_recommendation": summary(internal_samples),
        "first_result": first_result,
    }
    with tempfile.TemporaryDirectory(prefix="wordle-benchmark-") as temporary:
        cache = str(Path(temporary) / "patterns.bin")
        progress("Measuring uncached dynamic ranking and matrix construction...")
        dynamic["no_cache"] = run(["bench", "--no-cache", *native_options])
        progress("Measuring a new disk cache, then loading the same cache...")
        actual = ["bench", "--cache", cache, *native_options]
        displayed = ["bench", "--cache", "<temporary-cache>/patterns.bin", *native_options]
        dynamic["cold_cache"] = run(actual, display_arguments=displayed)
        dynamic["warm_cache"] = run(actual, display_arguments=displayed)
        if dynamic["cold_cache"]["result"].get("cache_loaded") is not False:
            raise RuntimeError("new temporary feedback cache was unexpectedly already loaded")
        if dynamic["warm_cache"]["result"].get("cache_loaded") is not True:
            raise RuntimeError("second feedback-cache run did not load its cache")
        if args.skip_dynamic_evaluation:
            dynamic["evaluation"] = {"skipped": True, "reason": "--skip-dynamic-evaluation"}
        else:
            progress("Evaluating the dynamic engine on all historical answers...")
            dynamic["evaluation"] = run(["evaluate", "--dynamic", "--no-cache", "--json"], evaluation=True)
    result["dynamic"] = dynamic

    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        output = args.output.expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(encoded, encoding="utf-8")
        progress(f"Saved benchmark results to {output}")
    else:
        sys.stdout.write(encoded)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"benchmark error: {error}", file=sys.stderr)
        raise SystemExit(1)
