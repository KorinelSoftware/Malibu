#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
VERSIONS_FILE = REPO_ROOT / "conformance" / "versions.json"


def load_versions() -> dict:
    return json.loads(VERSIONS_FILE.read_text(encoding="utf-8"))


def resolve_checkout(env_name: str, fallback: str) -> Path:
    configured = os.environ.get(env_name)
    path = Path(configured).expanduser() if configured else Path.home() / fallback
    if not path.is_dir():
        raise SystemExit(f"{env_name} checkout not found: {path}")
    return path.resolve()


def resolve_binary(env_name: str, relative_path: str) -> Path:
    configured = os.environ.get(env_name)
    path = Path(configured).expanduser() if configured else REPO_ROOT / relative_path
    if not path.is_file():
        raise SystemExit(
            f"{env_name} binary not found: {path}\n"
            "Build it with: cmake --build --preset release"
        )
    return path.resolve()


def git_head(path: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def require_pinned_checkout(path: Path, key: str) -> None:
    expected = load_versions()[key]["commit"]
    actual = git_head(path)
    if actual != expected:
        raise SystemExit(
            f"{key} checkout is not pinned: expected {expected}, found {actual} at {path}"
        )
