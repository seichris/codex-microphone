#!/usr/bin/env python3
"""Compile actual voice_audio.c with deterministic host-only FreeRTOS/codec shims."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HEADERS = (
    "esp_err.h", "bsp/esp-bsp.h", "driver/i2s_std.h", "esp_codec_dev.h", "esp_log.h",
    "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/semphr.h", "freertos/task.h",
)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="voice-audio-test-") as tmp:
        root = Path(tmp)
        for name in HEADERS:
            header = root / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "platform.h"\n', encoding="utf-8")
        executable = root / "voice_audio_capture_test"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(root), "-I", str(HERE / "audio_stubs"),
            str(HERE / "voice_audio_capture_test.c"), "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
