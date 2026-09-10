#!/usr/bin/env python3
"""Small regression tests for the tracked-file secret scanner."""

from __future__ import annotations

import sys
import tempfile
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from check_no_secrets import scan_repository, scan_text  # noqa: E402


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
    # Reject private pairing state by its path, even without secret-shaped text.
    forbidden = ["bridge/.attention-pairing/store.json", ".attention-pairing/identity.json"]
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        public = root / "docs/attention-pairing.md"
        public.parent.mkdir()
        public.write_text("Pairing documentation contains no credentials.\n")
        with patch("check_no_secrets.tracked_paths", return_value=forbidden + ["docs/attention-pairing.md"]):
            findings = scan_repository(root)
        assert [item[0] for item in findings] == forbidden, findings
        assert all(item[2] == "secret-bearing file must not be tracked" for item in findings)
    print("secret-safety scanner tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
