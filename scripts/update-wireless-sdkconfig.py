#!/usr/bin/env python3
"""Apply pairing settings without discarding existing Wi-Fi/bridge configuration."""
import os
from pathlib import Path
import re
import sys
import tempfile


def update(defaults: Path, target: Path) -> None:
    if target.is_symlink():
        raise ValueError("Refusing to replace a symlinked sdkconfig")
    if not target.exists():
        return
    settings = [line for line in defaults.read_text().splitlines() if line.startswith("CONFIG_")]
    keys = {line.split("=", 1)[0] for line in settings}
    keys.update({"CONFIG_CODEX_ATTENTION_VOICE_TRANSPORT_USB", "CONFIG_CODEX_ATTENTION_VOICE_TRANSPORT_WIFI"})
    if "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC" in keys:
        keys.update({"CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC", "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC",
                     "CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC", "CONFIG_MBEDTLS_IRAM_8BIT_MEM_ALLOC"})
    preserved = []
    for line in target.read_text().splitlines():
        match = re.match(r"(?:# )?(CONFIG_\w+)(?:=| is not set)", line)
        if not match or match[1] not in keys:
            preserved.append(line)
    descriptor, temporary = tempfile.mkstemp(prefix=".sdkconfig.", dir=target.parent)
    try:
        with os.fdopen(descriptor, "w") as output:
            output.write("\n".join(preserved + settings) + "\n")
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


if __name__ == "__main__":
    update(Path(sys.argv[1]), Path(sys.argv[2]))
