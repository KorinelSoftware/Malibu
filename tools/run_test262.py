#!/usr/bin/env python3

from __future__ import annotations

import os
import random
import re
import resource
import subprocess
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from conformance_common import (
    require_pinned_checkout,
    resolve_binary,
    resolve_checkout,
)


TEST262 = resolve_checkout("MALIBU_TEST262_ROOT", ".test262")
HARNESS = TEST262 / "harness"
RUNNER = resolve_binary(
    "MALIBU_TEST262_RUNNER", "build/release/tools/malibu_js"
)
MEM_CAP = int(os.environ.get("MEMCAP_MB", "1024")) * 1024 * 1024
FRONTMATTER = re.compile(r"/\*---(.*?)---\*/", re.S)


def limit_process() -> None:
    try:
        resource.setrlimit(resource.RLIMIT_AS, (MEM_CAP, MEM_CAP))
    except (OSError, ValueError):
        pass


def metadata(source: str) -> dict:
    match = FRONTMATTER.search(source)
    if not match:
        return {"flags": [], "includes": [], "negative": None}
    body = match.group(1)
    result = {"flags": [], "includes": [], "negative": None}
    flags = re.search(r"flags:\s*\[([^\]]*)\]", body)
    if flags:
        result["flags"] = [
            value.strip() for value in flags.group(1).split(",") if value.strip()
        ]
    includes = re.search(r"includes:\s*\[([^\]]*)\]", body)
    if includes:
        result["includes"] = [
            value.strip()
            for value in includes.group(1).split(",")
            if value.strip()
        ]
    negative = re.search(r"negative:", body)
    if negative:
        error_type = re.search(r"type:\s*(\w+)", body[negative.end() :])
        result["negative"] = {
            "type": error_type.group(1) if error_type else ""
        }
    return result


def run_one(path: str) -> tuple[str, str, str]:
    try:
        source = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "skip", path, "read"
    meta = metadata(source)
    flags = meta["flags"]
    if "module" in flags or "async" in flags or "CanBlockIsFalse" in flags:
        return "skip", path, "flag"

    files = []
    if "raw" not in flags:
        files = [HARNESS / "assert.js", HARNESS / "sta.js"]
        files.extend(HARNESS / include for include in meta["includes"])
    files.append(Path(path))
    try:
        result = subprocess.run(
            [str(RUNNER), *(str(file) for file in files)],
            capture_output=True,
            text=True,
            timeout=10,
            preexec_fn=limit_process,
        )
    except subprocess.TimeoutExpired:
        return "fail", path, "TIMEOUT"
    except OSError:
        return "fail", path, "SPAWN"

    ok = result.returncode == 0
    if meta["negative"]:
        return ("pass" if not ok else "fail"), path, "NEG-NOT-THROWN"
    return ("pass" if ok else "fail"), path, result.stdout.strip()


def signature(info: str) -> str:
    parse = re.match(r"ERROR: [^:]*:\d+:\d+: (.*)", info)
    if parse:
        message = re.sub(r"'[^']*'", "'X'", parse.group(1))
        return "PARSE: " + message[:60]
    compile_error = re.match(r"ERROR: compile error: (.*)", info)
    if compile_error:
        return "COMPILE: " + compile_error.group(1)[:60]
    thrown = re.match(r"ERROR: (\w*Error): (.*)", info)
    if thrown:
        message = re.sub(r"'[^']*'", "'X'", thrown.group(2))
        message = re.sub(r'"[^"]*"', '"X"', message)
        return thrown.group(1) + ": " + message[:55]
    assertion = re.match(r"ERROR: undefined: (.*)", info)
    if assertion:
        return "ASSERT: " + assertion.group(1)[:55]
    return info[:65]


def discover(globs: list[str]) -> list[str]:
    files: list[str] = []
    for glob in globs:
        base = TEST262 / "test" / glob
        if base.is_file() and base.suffix == ".js":
            files.append(str(base))
            continue
        if not base.exists():
            raise SystemExit(f"Test262 path not found: {base}")
        files.extend(
            str(path)
            for path in base.rglob("*.js")
            if "_FIXTURE" not in path.name
        )
    return sorted(set(files))


def main() -> int:
    require_pinned_checkout(TEST262, "test262")
    random.seed(42)
    globs = sys.argv[1:] or ["language", "built-ins"]
    files = discover(globs)
    print(f"running {len(files)} tests over {globs}")

    results: Counter[str] = Counter()
    buckets: Counter[str] = Counter()
    examples: dict[str, str] = {}
    missing: Counter[str] = Counter()
    not_function: Counter[str] = Counter()
    workers = int(os.environ.get("MAXW", "4"))

    with ProcessPoolExecutor(max_workers=workers) as executor:
        for status, path, info in executor.map(run_one, files, chunksize=64):
            results[status] += 1
            if status != "fail":
                continue
            key = signature(info)
            buckets[key] += 1
            examples.setdefault(
                key, str(Path(path).relative_to(TEST262 / "test"))
            )
            reading = re.search(r"reading '([^']+)'", info)
            if reading:
                missing[reading.group(1)] += 1
            not_fn = re.search(r"ERROR: TypeError: (\S+) is not a function", info)
            if not_fn:
                not_function[not_fn.group(1)] += 1

    total = results["pass"] + results["fail"]
    print(
        f"pass={results['pass']} fail={results['fail']} "
        f"skip={results['skip']} "
        f"-> {100 * results['pass'] / max(1, total):.1f}%"
    )
    print("\n=== top failure signatures ===")
    for key, count in buckets.most_common(35):
        print(f"{count:5d}  {key}\n         e.g. {examples[key]}")
    print("\n=== missing prop (reading 'X') ===")
    for key, count in missing.most_common(25):
        print(f"{count:5d}  {key}")
    print("\n=== not-a-function ===")
    for key, count in not_function.most_common(20):
        print(f"{count:5d}  {key}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
