#!/usr/bin/env python3
"""Verify that the repository's release version is consistent everywhere."""

from __future__ import annotations

import argparse
import json
import os
import plistlib
import re
import subprocess
import sys
from pathlib import Path


SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$")


def repository_root() -> Path:
    try:
        return Path(
            subprocess.check_output(
                ["git", "rev-parse", "--show-toplevel"],
                text=True,
                stderr=subprocess.DEVNULL,
            ).strip()
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"cannot find the repository root: {error}") from error


def read_package_version(path: Path) -> str:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)["version"]


def expected_version(root: Path) -> str:
    value = (root / "VERSION").read_text(encoding="utf-8").strip()
    if not SEMVER_RE.fullmatch(value):
        raise ValueError(f"VERSION is not valid semver: {value!r}")
    return value


def check(root: Path, tag: str | None = None) -> list[str]:
    version = expected_version(root)
    errors: list[str] = []

    with (root / "macos/Info.plist").open("rb") as handle:
        plist = plistlib.load(handle)
    if plist.get("CFBundleShortVersionString") != version:
        errors.append("macos/Info.plist: CFBundleShortVersionString does not match VERSION")

    for package_path in (root / "bridge/package.json", root / "bridge/package-lock.json"):
        if read_package_version(package_path) != version:
            errors.append(f"{package_path.relative_to(root)}: version does not match VERSION")

    app_server = (root / "bridge/src/codex-app-server.mjs").read_text(encoding="utf-8")
    if f"version: '{version}'" not in app_server:
        errors.append("bridge/src/codex-app-server.mjs: client version does not match VERSION")

    attention_client = (root / "firmware/main/attention_client.c").read_text(encoding="utf-8")
    if f"codex-esp32-display/{version}" not in attention_client:
        errors.append("firmware/main/attention_client.c: user agent does not match VERSION")

    if tag is None:
        tag = os.environ.get("GITHUB_REF_NAME")
    if tag and tag != f"v{version}":
        errors.append(f"tag {tag!r} does not match v{version}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", help="also require this tag to equal vVERSION")
    args = parser.parse_args()
    root = repository_root()
    try:
        errors = check(root, args.tag)
    except (OSError, KeyError, ValueError, json.JSONDecodeError, plistlib.InvalidFileException) as error:
        print(f"version check failed: {error}", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"version check failed: {error}", file=sys.stderr)
        return 1
    print(f"version {expected_version(root)} is consistent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
