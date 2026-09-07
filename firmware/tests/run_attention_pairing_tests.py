#!/usr/bin/env python3
"""Compile production pairing/client code with explicit host platform adapters."""
from pathlib import Path
import ctypes.util
import hashlib
import hmac
import os
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HEADERS = ["esp_err.h", "esp_heap_caps.h", "esp_hmac.h", "esp_random.h", "freertos/FreeRTOS.h", "freertos/semphr.h",
           "mbedtls/md.h", "mbedtls/x509_crt.h", "nvs.h", "nvs_flash.h", "nvs_sec_provider.h", "sdkconfig.h",
           "cJSON.h", "esp_http_client.h", "esp_netif.h", "esp_timer.h", "esp_tls.h", "mdns.h"]


def run() -> None:
    with tempfile.TemporaryDirectory(prefix="attention-host-") as directory:
        output = Path(directory)
        for name in HEADERS:
            header = output / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "attention_host_platform.h"\n', encoding="utf-8")
        config = output / "openssl.cnf"
        config.write_text("[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n[dn]\nCN=attention-22222222222222222222222222222222.local\n[ext]\nsubjectAltName=DNS:attention-22222222222222222222222222222222.local\nbasicConstraints=critical,CA:TRUE\n")
        subprocess.run(["openssl", "req", "-new", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
            "-nodes", "-keyout", str(output / "key.pem"), "-out", str(output / "cert.pem"), "-days", "3650",
            "-config", str(config)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        common = shlex.split(os.environ.get("CC", "cc")) + ["-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
            "-I", str(output), "-I", str(HERE), "-pthread"]
        sources = [str(HERE / "attention_host_platform.c"), str(HERE / "../main/attention_pairing_record.c"),
            str(HERE / "../main/attention_connection_status.c")]
        unit = output / "pairing-test"
        subprocess.run(common + [str(HERE / "attention_pairing_test.c"), str(HERE / "../main/attention_confirmation.c")]
            + sources + ["-lcrypto", "-o", str(unit)], check=True)
        canonical = "\n".join(["codex-attention-auth-v1", "2" * 32, "1" * 32, "1", "b" * 64, "c" * 64])
        proof = hmac.new(bytes.fromhex("a" * 64), canonical.encode(), hashlib.sha256).hexdigest()
        subprocess.run([str(unit), str(output / "cert.pem"), proof], check=True, timeout=15)
        curl, cjson = ctypes.util.find_library("curl"), ctypes.util.find_library("cjson")
        if not curl or not cjson:
            raise RuntimeError("Install libcurl and libcjson development/runtime libraries for the HTTPS integration test")
        client = output / "network-test"
        subprocess.run(common + [str(HERE / "attention_network_test.c"), str(HERE / "attention_host_network.c"),
            str(HERE / "../main/attention_pairing.c"), str(HERE / "../main/attention_connection.c"),
            str(HERE / "../main/attention_client.c")] + sources
            + ["-lcrypto", f"-l:{curl}", f"-l:{cjson}", "-o", str(client)], check=True)
        subprocess.run(["node", str(ROOT / "scripts/test-attention-integration.mjs"), str(client)], check=True, timeout=60)


if __name__ == "__main__":
    run()
