#!/usr/bin/env python3
"""Exercise pairing -> Kconfig -> compiled C -> PEM with synthetic credentials."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import kconfiglib

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


with tempfile.TemporaryDirectory(prefix="wireless-provision-test-") as directory:
    tmp = Path(directory)
    bundle = tmp / "pairing.json"
    run("bash", str(ROOT / "scripts/create-wireless-pairing-bundle.sh"),
        "--host", "192.0.2.1", "--server-name", "microphone.test", "--output", str(bundle))
    pairing = json.loads(bundle.read_text())
    certificate = tmp / "expected.pem"
    certificate.write_text(pairing["serverCertificatePEM"])
    assert "DNS:microphone.test" in run("openssl", "x509", "-in", str(certificate), "-noout", "-text")
    config = tmp / "sdkconfig"
    config.write_text('CONFIG_CODEX_ATTENTION_WIFI_SSID="preserved"\n'
                      'CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y\n'
                      '# CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK is not set\n'
                      'CONFIG_CODEX_ATTENTION_BRIDGE_TOKEN="preserved-token"\n'
                      'CONFIG_CODEX_ATTENTION_WIRELESS_CREDENTIAL="old"\n'
                      '# CONFIG_CODEX_ATTENTION_VOICE_TRANSPORT_AUTO is not set\n'
                      'CONFIG_CODEX_ATTENTION_VOICE_TRANSPORT_WIFI=y\n')
    run("bash", str(ROOT / "scripts/provision-wireless-microphone.sh"), str(bundle), str(tmp))
    assert 'CONFIG_CODEX_ATTENTION_WIFI_SSID="preserved"' in config.read_text()
    assert 'CONFIG_CODEX_ATTENTION_BRIDGE_TOKEN="preserved-token"' in config.read_text()
    assert 'CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y' in config.read_text()
    assert 'CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y' in config.read_text()
    assert '# CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK is not set' not in config.read_text()
    assert 'CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y' not in config.read_text()
    assert config.stat().st_mode & 0o777 == 0o600
    kconfig = kconfiglib.Kconfig(str(ROOT / "firmware/main/Kconfig.projbuild"), warn=False)
    kconfig.load_config(str(config))
    assert kconfig.syms["CODEX_ATTENTION_WIRELESS_CREDENTIAL"].str_value == pairing["credential"]
    assert kconfig.syms["CODEX_ATTENTION_VOICE_TRANSPORT_AUTO"].str_value == "y"
    kconfig.write_autoconf(str(tmp / "sdkconfig.h"))
    source = tmp / "decode.c"
    source.write_text('''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "wireless_certificate.h"
int main(void) {
    char output[16384];
    assert(!wireless_certificate_decode("invalid", output, sizeof(output)));
    assert(!wireless_certificate_decode("\\\\x", output, sizeof(output)));
    assert(!wireless_certificate_decode(CONFIG_CODEX_ATTENTION_WIRELESS_CA_PEM, output, 2));
    assert(wireless_certificate_decode(CONFIG_CODEX_ATTENTION_WIRELESS_CA_PEM, output, sizeof(output)));
    fputs(output, stdout);
    return 0;
}
''')
    run(os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror", "-I", str(tmp),
        "-I", str(ROOT / "firmware/main"), str(source),
        str(ROOT / "firmware/main/wireless_certificate.c"), "-o", str(tmp / "decode"))
    actual = run(str(tmp / "decode"))
    assert actual == pairing["serverCertificatePEM"]
    print("PASS pairing certificate survives real Kconfig and C compilation byte-for-byte")
    print("PASS existing pairing rotates, transport updates, Wi-Fi/bridge settings remain, mode 600")
    print("PASS certificate SAN matches the configured TLS name when host differs")
    pairing["host"] = "host@untrusted.test"
    bundle.write_text(json.dumps(pairing))
    before = config.read_bytes()
    result = subprocess.run(["bash", str(ROOT / "scripts/provision-wireless-microphone.sh"),
                             str(bundle), str(tmp)], capture_output=True)
    assert result.returncode != 0 and before == config.read_bytes()
    print("PASS malformed endpoint cannot overwrite active provisioning")
