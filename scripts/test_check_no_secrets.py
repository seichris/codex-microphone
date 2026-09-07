#!/usr/bin/env python3
"""Small regression tests for the tracked-file secret scanner."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from check_no_secrets import scan_text  # noqa: E402


def assert_clean(path: str, text: str) -> None:
    findings = scan_text(path, text)
    assert not findings, findings


def assert_rejected(path: str, text: str) -> None:
    findings = scan_text(path, text)
    assert findings, "expected a secret-safety finding"


def main() -> int:
    config_prefix = "CONFIG_CODEX_ATTENTION_"
    assert_clean("firmware/sdkconfig.defaults", f'{config_prefix + "WIFI_SSID"}="CHANGE_ME"\n')
    assert_clean("bridge/test/example.test.mjs", "const " + "token" + " = 'abcdefghijklmnopqrstuvwxyz123456';\n")
    assert_clean("docs/example.md", "Authorization: " + "Bearer <token>\n")
    assert_clean(".env.example", "WIFI_" + "PASSWORD=CHANGE_ME\n")
    assert_rejected("firmware/sdkconfig", f'{config_prefix + "WIFI_PASSWORD"}="a-real-password"\n')
    assert_rejected("bridge/src/example.mjs", "const " + "token" + " = 'this-is-a-real-long-lived-token-value';\n")
    assert_rejected("bridge/src/example.mjs", "const " + "token" + " = `this-is-a-real-template-token-value`;\n")
    assert_rejected(".env", "WIFI_" + "PASSWORD=real-wifi-password-value\n")
    assert_rejected("config/production.json", '"wifi' + 'Password' + '": "' + 'real-wifi-password-value"\n')
    assert_rejected("docs/example.md", "Authorization: " + "Bearer real-bridge-token-value-1234567890\n")
    print("secret-safety scanner tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
