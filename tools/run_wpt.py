#!/usr/bin/env python3

from __future__ import annotations

import os
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


WPT = resolve_checkout("MALIBU_WPT_ROOT", ".wpt")
RUNNER = resolve_binary(
    "MALIBU_WPT_RUNNER", "build/release/tools/malibu_wpt"
)
MEM_CAP = int(os.environ.get("MEMCAP_MB", "1024")) * 1024 * 1024


def limit_process() -> None:
    try:
        resource.setrlimit(resource.RLIMIT_AS, (MEM_CAP, MEM_CAP))
    except (OSError, ValueError):
        pass


def run_one(path: str) -> tuple[str, str, int, int]:
    try:
        result = subprocess.run(
            [str(RUNNER), str(WPT), path],
            capture_output=True,
            text=True,
            timeout=15,
            preexec_fn=limit_process,
        )
    except subprocess.TimeoutExpired:
        return path, "TIMEOUT", 0, 0
    except OSError:
        return path, "SPAWN", 0, 0

    lines = result.stdout.strip().splitlines()
    line = lines[-1] if lines else ""
    if not line.startswith("RESULT"):
        return path, "NORESULT", 0, 0
    parts = line.split()
    status = parts[1] if len(parts) > 1 else "?"
    try:
        passed, failed = int(parts[2]), int(parts[3])
    except (IndexError, ValueError):
        passed, failed = 0, 0
    return path, status, passed, failed


def discover(globs: list[str]) -> list[str]:
    files: list[str] = []
    for glob in globs:
        base = WPT / glob
        if base.is_file() and base.suffix == ".html":
            files.append(str(base))
            continue
        if not base.exists():
            raise SystemExit(f"WPT path not found: {base}")
        files.extend(
            str(path)
            for path in base.rglob("*.html")
            if not path.name.endswith("-manual.html")
        )
    return sorted(set(files))


def main() -> int:
    require_pinned_checkout(WPT, "wpt")
    globs = sys.argv[1:] or ["dom", "html/dom"]
    files = discover(globs)
    print(f"running {len(files)} WPT files over {globs}")

    total_pass = total_fail = 0
    files_ok = files_partial = files_none = 0
    buckets: Counter[str] = Counter()
    examples: dict[str, str] = {}
    workers = int(os.environ.get("MAXW", "4"))

    with ProcessPoolExecutor(max_workers=workers) as executor:
        for path, status, passed, failed in executor.map(
            run_one, files, chunksize=16
        ):
            total_pass += passed
            total_fail += failed
            if status == "0" and failed == 0 and passed > 0:
                files_ok += 1
            elif passed > 0 or failed > 0:
                files_partial += 1
            else:
                files_none += 1
                buckets[status] += 1
                examples.setdefault(status, str(Path(path).relative_to(WPT)))

    total = total_pass + total_fail
    print(
        f"\nsubtests: pass={total_pass} fail={total_fail} "
        f"-> {100 * total_pass / max(1, total):.1f}%"
    )
    print(
        f"files: clean={files_ok} partial={files_partial} no-result={files_none}"
    )
    print("\nno-result buckets:")
    for status, count in buckets.most_common(12):
        print(f"  {count:5d}  hs={status!r}  e.g. {examples[status]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
