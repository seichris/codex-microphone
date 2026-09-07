#!/usr/bin/env python3
"""Link production capture, wireless lifecycle and framing with deterministic shims."""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
HEADERS = (
    "esp_err.h", "bsp/esp-bsp.h", "driver/i2s_std.h", "esp_codec_dev.h", "esp_log.h",
    "esp_heap_caps.h", "esp_wifi.h", "esp_timer.h", "esp_websocket_client.h", "cJSON.h",
    "mbedtls/x509_crt.h", "freertos/FreeRTOS.h", "freertos/queue.h",
    "freertos/semphr.h", "freertos/task.h", "freertos/event_groups.h", "sdkconfig.h",
)

def main(mode: str) -> None:
    if mode not in ("audio", "wireless"):
        raise ValueError("expected audio or wireless")
    with tempfile.TemporaryDirectory(prefix="wireless-capture-test-") as directory:
        root = Path(directory)
        for name in HEADERS:
            header = root / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "platform.h"\n', encoding="utf-8")
        binary = root / "wireless-capture-test"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(root), "-I", str(HERE / "integrated_stubs"),
            str(HERE / "wireless_capture_integration_test.c"),
            str(HERE / "../main/wireless_certificate.c"), "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(binary), mode], check=True, timeout=15)

if __name__ == "__main__":
    main(sys.argv[1])
