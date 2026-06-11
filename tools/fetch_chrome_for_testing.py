#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import shutil
import urllib.request
import zipfile
from pathlib import Path

from conformance_common import REPO_ROOT, load_versions


def main() -> int:
    chrome = load_versions()["chrome_for_testing"]
    target_root = REPO_ROOT / ".conformance" / "chrome-for-testing"
    install_dir = target_root / chrome["version"]
    executable = install_dir / "chrome-linux64" / "chrome"
    if executable.is_file():
        print(executable)
        return 0

    target_root.mkdir(parents=True, exist_ok=True)
    archive = target_root / f"chrome-linux64-{chrome['version']}.zip"
    partial = archive.with_suffix(".zip.part")
    print(f"downloading {chrome['linux64_url']}")
    with urllib.request.urlopen(chrome["linux64_url"], timeout=60) as response:
        with partial.open("wb") as output:
            shutil.copyfileobj(response, output)
    partial.replace(archive)
    print(f"downloaded sha256={hashlib.sha256(archive.read_bytes()).hexdigest()}")

    install_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as bundle:
        bundle.extractall(install_dir)
    executable.chmod(executable.stat().st_mode | 0o111)
    print(executable)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
