#!/usr/bin/env python3
"""Exercise the actual UART provisioning CLI without a serial driver or hardware."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("pair_attention", Path(__file__).with_name("pair-attention.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SerialStream:
    def __init__(self, chunks):
        self.chunks = iter(chunks)
    def read_until(self, *args):
        return next(self.chunks, b"")


def reader(chunks):
    device = module.DeviceSerial.__new__(module.DeviceSerial)
    device.stream, device.buffer, device.discard = SerialStream(chunks), bytearray(), False
    return device


class ProvisioningTests(unittest.TestCase):
    def test_full_width_ssid_psk_and_utf8(self):
        self.assertEqual(module.wifi_credentials("s" * 32, "A" * 64), ("s" * 32, "A" * 64))
        self.assertEqual(module.wifi_credentials("测试", "password"), ("测试", "password"))
        for ssid, password in [("", ""), ("长" * 11, "password"), ("s", "x" * 64), ("a\0b", "password"), ("s", "short")]:
            with self.assertRaises(module.PairingError): module.wifi_credentials(ssid, password)

    def test_split_uart_reply_survives_read_timeout(self):
        device = reader([b"boot log\n", b"CODEX-ATTEN", b"", b'TION {"version":1,', b'"result":"paired"}\n'])
        self.assertEqual(device.reply({"paired"}, timeout=0.1)["result"], "paired")

    def test_oversize_is_discarded_to_newline_before_next_frame(self):
        device = reader([b"x" * (module.MAX_LINE + 1), b"garbage\n", b'CODEX-ATTENTION {"version":1,"result":"paired"}\n'])
        self.assertEqual(device.reply({"paired"}, timeout=0.1)["result"], "paired")

    def test_unknown_device_error_is_not_echoed(self):
        device = reader([b'CODEX-ATTENTION {"version":1,"result":"secret-from-untrusted-peer"}\n'])
        with self.assertRaises(module.PairingError) as error: device.reply({"paired"}, timeout=0.1)
        self.assertNotIn("secret-from-untrusted-peer", str(error.exception))

    def test_expired_confirmation_is_not_assumed_successful(self):
        device = reader([])
        with self.assertRaises(module.PairingError): device.reply({"paired"}, timeout=0.001)

    def test_reset_on_wrong_mac_preserves_pending_device(self):
        class Admin:
            def request(self, *args): return {"bridgeId": "2" * 32}
        class Device:
            def send(self, *args): raise AssertionError("must not clear device")
        info = {"state": 2, "bridgeId": "1" * 32, "deviceId": "3" * 32, "resetNonce": "a" * 64}
        with self.assertRaisesRegex(module.PairingError, "Reset remains pending"):
            module.finish_reset(Device(), Admin(), info)

    def test_pairing_requires_fresh_confirmation_and_activation_waits_for_durable_reply(self):
        from ssl import DER_cert_to_PEM_cert
        # The CLI checks PEM framing/fingerprint; firmware validates real X.509.
        bundle = {"version": 1, "deviceId": "1" * 32, "bridgeId": "2" * 32, "secret": "a" * 64,
                  "certificate": DER_cert_to_PEM_cert(b"fixture"), "generation": 1, "port": 5182,
                  "fallbackHost": "", "provisionedAt": 1800000000}
        events = []
        class Admin:
            def request(self, action="", data=None):
                events.append(action)
                return bundle if action == "/prepare" else {"bridgeId": bundle["bridgeId"]}
        class Device:
            def send(self, data):
                events.append("send")
                self.data = data
            def reply(self, accepted, timeout=10):
                result = next(iter(accepted)); events.append(result)
                if result == "paired": raise module.PairingError("not confirmed")
                return {"result": result}
            def hello(self): return {"state": 0}
        device = Device()
        with patch("builtins.input", return_value="SSID"), patch.object(module.getpass, "getpass", return_value="wifi-test-secret"), contextlib.redirect_stdout(io.StringIO()) as output:
            with self.assertRaises(module.PairingError):
                module.enroll(device, Admin(), {"state": 0, "candidateId": bundle["deviceId"]})
        self.assertNotIn("/activate", events)
        self.assertNotIn(bundle["secret"], output.getvalue())
        self.assertNotIn("wifi-test-secret", output.getvalue())
        self.assertEqual(device.data["ssid"], "SSID")
        self.assertEqual(device.data["command"], "pair")


if __name__ == "__main__": unittest.main()
