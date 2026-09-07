#!/usr/bin/env python3
"""Compile actual WSS lifecycle callbacks with host-only transport/JSON fixtures."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HEADERS = (
    "esp_err.h", "esp_log.h", "esp_timer.h", "esp_websocket_client.h", "cJSON.h",
    "freertos/FreeRTOS.h", "freertos/event_groups.h", "freertos/semphr.h",
    "freertos/task.h", "sdkconfig.h",
)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="wireless-lifecycle-test-") as tmp:
        root = Path(tmp)
        for name in HEADERS:
            header = root / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "platform.h"\n', encoding="utf-8")
        executable = root / "wireless_microphone_lifecycle_test"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(root), "-I", str(HERE / "wireless_stubs"),
            str(HERE / "wireless_microphone_lifecycle_test.c"), "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
