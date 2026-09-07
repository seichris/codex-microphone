#!/usr/bin/env python3
"""Exercise production Wi-Fi configuration and event/retry policy on the host."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
headers = ["esp_err.h", "esp_check.h", "esp_event.h", "esp_log.h", "esp_netif.h",
           "esp_sntp.h", "esp_wifi.h", "esp_timer.h", "freertos/FreeRTOS.h",
           "freertos/event_groups.h", "sdkconfig.h"]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for name in headers:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "wifi_test_platform.h"\n')
    output = root / "wifi-test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(root),
                    "-I", str(HERE), str(HERE / "wifi_manager_test.c"), "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)
