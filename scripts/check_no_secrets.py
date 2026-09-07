#!/usr/bin/env python3
"""Reject tracked Wi-Fi, bridge-token, and secret-bearing release inputs.

This intentionally scans Git's tracked file list rather than the whole working
tree. Local firmware configurations and bridge/config.json remain useful for
development, but they must never become part of a commit or release source.
The scanner reports file names and line numbers only; it never prints a match.
"""

from __future__ import annotations

import argparse
import ast
import fnmatch
import re
import subprocess
import sys
from pathlib import Path


SENSITIVE_CONFIG_KEYS = {
    "CONFIG_CODEX_ATTENTION_WIFI_SSID",
    "CONFIG_CODEX_ATTENTION_WIFI_PASSWORD",
    "CONFIG_CODEX_ATTENTION_BRIDGE_TOKEN",
    "CONFIG_CODEX_ATTENTION_WIRELESS_CREDENTIAL",
    "CONFIG_CODEX_ATTENTION_WIRELESS_CA_PEM",
}

# These values are deliberately deterministic test fixtures already used by
# the host/firmware tests. Keeping the allowlist exact prevents a real value
# from being hidden behind a broad "test" path exemption.
SAFE_FIXTURE_VALUES = {
    "abcdefghijklmnopqrstuvwxyz123456",
    "preserved",
    "preserved-token",
    "old",
    "test certificate",
    "12345678901234567890123456789012",
    "0123456789abcdef" * 4,
    "a" * 64,
}

PLACEHOLDER_RE = re.compile(
    r"^(?:<[^>]+>|CHANGE[_ -]?ME|YOUR[_ -]?[A-Z0-9_-]+|REPLACE[_ -]?(?:ME|THIS)|"
    r"REPLACE[_ -]WITH[_ -](?:A[_ -])?(?:LONG[_ -])?(?:RANDOM[_ -])?(?:TOKEN|VALUE|PASSWORD|SSID)|"
    r"GENERATED[_ -]BY[_ -](?:NPM[_ -]RUN[_ -]SETUP|SETUP)|"
    r"EXAMPLE(?:[_ -]?(?:VALUE|TOKEN|PASSWORD|SSID))?|"
    r"TEST(?:[_ -]?(?:VALUE|TOKEN|PASSWORD|SSID))?|"
    r"FIXTURE(?:[_ -]?(?:VALUE|TOKEN|PASSWORD|SSID))?)$",
    re.IGNORECASE,
)

CONFIG_ASSIGNMENT_RE = re.compile(
    r"(?P<key>CONFIG_CODEX_ATTENTION_[A-Z0-9_]+)\s*(?:=|:)\s*"
    r"(?P<value>\"(?:\\.|[^\"])*\"|'(?:\\.|[^'])*'|[^\s,;}]+)"
)

INLINE_ASSIGNMENT_RE = re.compile(
    r"(?P<key>\b(?:bridgeToken|wifiPassword|wifiPassphrase|wifiCredential|wirelessCredential|"
    r"token|password|passphrase|secret|credential|ssid|"
    r"api[_-]?key)\b)[\"']?\s*(?:=|:)\s*"
    r"(?P<value>\"(?:\\.|[^\"])*\"|'(?:\\.|[^'])*'|`(?:\\.|[^`])*`)",
    re.IGNORECASE,
)

UNQUOTED_SENSITIVE_ASSIGNMENT_RE = re.compile(
    r"(?P<key>\b(?:WIFI[_-]?(?:SSID|PASSWORD|PASS|PASSPHRASE)|BRIDGE[_-]?TOKEN|"
    r"CODEX[_-]?ATTENTION[_-]?TOKEN|WIRELESS[_-]?CREDENTIAL)\b)[\"']?\s*(?:=|:)\s*"
    r"(?P<value>[^\s,;}#]+)",
    re.IGNORECASE,
)

BEARER_RE = re.compile(r"\bAuthorization\s*:\s*Bearer\s+(?P<value>[^\s\"']+)", re.IGNORECASE)

FORBIDDEN_PATHS = {
    "bridge/config.json",
    "firmware/sdkconfig",
    "firmware/sdkconfig.wireless.defaults",
}


def _literal(raw: str) -> str:
    raw = raw.strip()
    if raw[:1] == "`" and raw[-1:] == "`":
        return raw[1:-1]
    if raw[:1] in {"'", '"'}:
        try:
            value = ast.literal_eval(raw)
            return value if isinstance(value, str) else raw
        except (SyntaxError, ValueError):
            return raw[1:-1] if raw[-1:] == raw[:1] else raw
    return raw


def _is_placeholder(value: str) -> bool:
    return not value or bool(PLACEHOLDER_RE.fullmatch(value))


def _is_dynamic(value: str) -> bool:
    # Shell, CMake, and JavaScript interpolation is not a committed secret.
    return (
        value.startswith(("$", "${", "%", "<", "process.", "os.", "env.", "getenv("))
        or "${" in value
        or value in {"try", "String", "String?", "nil", "None", "true", "false"}
    )


def _safe_fixture_for_key(path: str, key: str, value: str) -> bool:
    normalized_path = path.lower()
    is_test_fixture_path = (
        "/test/" in f"/{normalized_path}/"
        or "/tests/" in f"/{normalized_path}/"
        or normalized_path.startswith("scripts/test")
    )
    if _is_placeholder(value) or (is_test_fixture_path and value in SAFE_FIXTURE_VALUES):
        return True
    # The certificate fixture is intentionally represented as escaped PEM and
    # should remain test-only. Do not generalize this exception to credentials.
    return (
        key == "CONFIG_CODEX_ATTENTION_WIRELESS_CA_PEM"
        and "/tests/" in f"/{path}/"
        and "BEGIN CERTIFICATE" in value
        and "fixture" in value
    )


def scan_text(path: str, text: str) -> list[tuple[str, int, str]]:
    findings: list[tuple[str, int, str]] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        for match in CONFIG_ASSIGNMENT_RE.finditer(line):
            key = match.group("key")
            if key not in SENSITIVE_CONFIG_KEYS:
                continue
            value = _literal(match.group("value"))
            if not _safe_fixture_for_key(path, key, value) and not _is_dynamic(value):
                findings.append((path, line_number, f"sensitive config assignment ({key})"))

        for match in INLINE_ASSIGNMENT_RE.finditer(line):
            value = _literal(match.group("value"))
            key = match.group("key")
            minimum_length = 8 if key.lower() in {"password", "passphrase"} else 24
            if len(value) >= minimum_length and not _safe_fixture_for_key(path, key, value) and not _is_dynamic(value):
                findings.append((path, line_number, "secret-shaped inline assignment"))

        for match in UNQUOTED_SENSITIVE_ASSIGNMENT_RE.finditer(line):
            value = _literal(match.group("value"))
            key = match.group("key")
            if not _safe_fixture_for_key(path, key, value) and not _is_dynamic(value):
                findings.append((path, line_number, "unquoted sensitive assignment"))

        for match in BEARER_RE.finditer(line):
            value = match.group("value")
            if len(value) >= 24 and not _safe_fixture_for_key(path, "token", value):
                findings.append((path, line_number, "literal bearer token"))

    return findings


def tracked_paths(root: Path) -> list[str]:
    output = subprocess.check_output(["git", "ls-files", "-z"], cwd=root)
    return [path for path in output.decode().split("\0") if path]


def scan_repository(root: Path) -> list[tuple[str, int, str]]:
    findings: list[tuple[str, int, str]] = []
    for relative_path in tracked_paths(root):
        if (relative_path in FORBIDDEN_PATHS or ".attention-pairing" in Path(relative_path).parts
            or fnmatch.fnmatch(relative_path, "wireless-pairing*.json") or relative_path.endswith(".p12")):
            findings.append((relative_path, 1, "secret-bearing file must not be tracked"))
            continue
        path = root / relative_path
        try:
            data = path.read_bytes()
        except OSError as error:
            findings.append((relative_path, 1, f"cannot read tracked file ({error})"))
            continue
        if b"\0" in data:
            continue
        findings.extend(scan_text(relative_path, data.decode("utf-8", errors="replace")))
    return findings


def repository_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    root = (args.root or repository_root()).resolve()
    findings = scan_repository(root)
    if findings:
        print("secret-safety check failed; matches are redacted:", file=sys.stderr)
        for path, line_number, reason in findings:
            print(f"  {path}:{line_number}: {reason}", file=sys.stderr)
        return 1
    print("secret-safety check passed for tracked files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
