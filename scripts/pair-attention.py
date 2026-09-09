#!/usr/bin/env python3
"""Physically confirmed attention pairing over native USB-C; never burn eFuses."""
from __future__ import annotations

import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import re
import ssl
import sys
import time
import urllib.error
import urllib.request

PREFIX = b"CODEX-ATTENTION "
MAX_LINE = 8192
HEX_ID = re.compile(r"[0-9a-f]{32}\Z")
HEX_SECRET = re.compile(r"[0-9a-f]{64}\Z")
ROOT = Path(__file__).resolve().parents[1]


class PairingError(Exception):
    """A bounded, non-secret diagnostic suitable for a terminal."""


def wifi_credentials(ssid: str, password: str) -> tuple[str, str]:
    if "\0" in ssid or not 1 <= len(ssid.encode("utf-8")) <= 32:
        raise PairingError("Wi-Fi SSID must contain 1–32 UTF-8 bytes, without NUL.")
    size = len(password.encode("utf-8"))
    if "\0" in password or (size != 0 and not 8 <= size <= 64):
        raise PairingError("Wi-Fi password must be empty, 8–63 bytes, or a 64-digit hexadecimal PSK.")
    if size == 64 and re.fullmatch(r"[0-9a-fA-F]{64}", password) is None:
        raise PairingError("A 64-byte Wi-Fi PSK must be hexadecimal.")
    return ssid, password


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, new_url):
        return None  # Never forward the local administrative bearer elsewhere.


class LocalAdmin:
    def __init__(self, config_path: Path) -> None:
        self.config_path = config_path
        # Never leak a loopback administrative bearer through HTTP_PROXY.
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def request(self, action: str = "", body: dict | None = None) -> dict:
        try:
            config = json.loads(self.config_path.read_text(encoding="utf-8"))
            token = os.environ.get("CODEX_ATTENTION_TOKEN", config.get("token", ""))
            port = int(os.environ.get("CODEX_ATTENTION_PORT", config.get("port", 5180)))
            if not isinstance(token, str) or len(token) < 24 or not 1 <= port <= 65535:
                raise ValueError()
        except (OSError, ValueError, TypeError):
            raise PairingError("Cannot read a valid local bridge configuration. Start the bridge with npm run setup/start.") from None
        url = f"http://127.0.0.1:{port}/api/v1/admin/devices{action}"
        request = urllib.request.Request(url, data=None if body is None else json.dumps(body).encode(),
            headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
        try:
            with self.opener.open(request, timeout=10) as response:
                data = response.read(16 * 1024 + 1)
                if len(data) > 16 * 1024:
                    raise PairingError("Local bridge response exceeded the size limit.")
                result = json.loads(data)
                if not isinstance(result, dict):
                    raise ValueError()
                return result
        except urllib.error.HTTPError as error:
            raise PairingError(f"Local bridge rejected the request (HTTP {error.code}).") from None
        except (OSError, ValueError):
            raise PairingError("Paired local bridge unavailable or returned an invalid response.") from None


def select_usb_port(ports) -> str:
    candidates = [p.device for p in ports
                  if p.manufacturer == "Codex ESP32 Display"
                  and (p.interface in (None, "Attention pairing"))]
    if len(candidates) != 1:
        raise PairingError("Connect one USB-C board running USB pairing firmware, or select its CDC port with --port.")
    return candidates[0]


class DeviceSerial:
    def __init__(self, port: str) -> None:
        try:
            import serial
        except ImportError:
            raise PairingError("Install the serial dependency: python3 -m pip install -r scripts/requirements-attention.txt") from None
        self.buffer = bytearray()
        self.discard = False
        try:
            self.stream = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2)
            # The application CDC interface uses DTR to mark a pairing session.
            # This is not the ESP ROM downloader; RTS stays deasserted.
            self.stream.dtr = True
            self.stream.rts = False
            self.stream.port = port
            self.stream.open()
        except (OSError, serial.SerialException):
            raise PairingError("Cannot open the USB pairing port. Connect USB-C and start the application firmware.") from None

    def close(self) -> None:
        self.stream.close()

    def send(self, value: dict) -> None:
        data = json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode("ascii") + b"\n"
        if len(data) > MAX_LINE:
            raise PairingError("Pairing request exceeded the serial limit.")
        self.stream.write(data)
        self.stream.flush()

    def reply(self, accepted: set[str], timeout: float = 10) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.stream.read_until(b"\n", MAX_LINE + 1)
            if not chunk:
                continue
            if not self.discard:
                self.buffer.extend(chunk)
                if len(self.buffer) > MAX_LINE:
                    self.buffer.clear()
                    self.discard = True
            if not chunk.endswith(b"\n"):
                continue  # Preserve a reply split across serial read timeouts.
            line = bytes(self.buffer)
            self.buffer.clear()
            discard, self.discard = self.discard, False
            if discard or not line.startswith(PREFIX):
                continue
            try:
                result = json.loads(line[len(PREFIX):])
                if not isinstance(result, dict) or result.get("version") != 1:
                    continue
            except (ValueError, UnicodeDecodeError):
                continue
            code = result.get("result")
            if code in accepted:
                return result
            if code not in {"hello", "confirm_on_device"}:
                # Do not echo arbitrary data from a serial peer into the terminal.
                raise PairingError("Device rejected or cancelled the pairing operation. Check its display and retry.")
        raise PairingError("No confirmed device reply. Check its display and USB-C connection; no reset is assumed complete.")

    def hello(self) -> dict:
        self.send({"command": "hello"})
        result = self.reply({"hello"})
        if not HEX_ID.fullmatch(str(result.get("candidateId", ""))) or result.get("state") not in (0, 1, 2):
            raise PairingError("Invalid device identity response.")
        return result


def finish_reset(device: DeviceSerial, admin: LocalAdmin, info: dict) -> None:
    if info["state"] == 0:
        return
    if info["state"] == 1:
        device.send({"command": "reset"})
        device.reply({"confirm_on_device"})
        print("On the device: release BOOT, then hold it for 1.5 seconds to RESET. PWR cancels.")
        device.reply({"reset_pending"}, timeout=75)
        info = device.hello()
    if info["state"] == 0:  # paired LAN acknowledgement already completed it
        return
    nonce = info.get("resetNonce", "")
    if info["state"] != 2 or not HEX_SECRET.fullmatch(nonce):
        raise PairingError("Reset has not reached its durable pending state.")
    try:
        mac = admin.request()
        if mac.get("bridgeId") != info.get("bridgeId"):
            raise PairingError("This is not the originally paired Mac.")
        ack = admin.request("/revoke", {"deviceId": info["deviceId"], "nonce": nonce})
        if ack.get("deviceId") != info["deviceId"] or ack.get("nonce") != nonce or not HEX_SECRET.fullmatch(str(ack.get("proof", ""))):
            raise PairingError("Invalid bridge revocation acknowledgement.")
        device.send({"command": "reset-ack", "nonce": nonce, "proof": ack["proof"]})
        try:
            device.reply({"reset_complete"})
        except PairingError:
            # The LAN worker may have committed the same acknowledgement first.
            if device.hello()["state"] != 0:
                raise
        if device.hello()["state"] != 0:
            raise PairingError("Device has not committed reset completion.")
    except PairingError as error:
        raise PairingError(f"Reset remains pending and normal attention access is blocked locally. Reconnect the original Mac to finish. {error}") from None


def enroll(device: DeviceSerial, admin: LocalAdmin, info: dict, *, replace: bool = False) -> None:
    previous_id = info.get("deviceId") if info["state"] != 0 else None
    if info["state"] == 1 and not replace:
        raise PairingError("Device is already paired. Use --replace on its original Mac, or --reset before pairing another Mac.")
    if info["state"] != 0:
        finish_reset(device, admin, info)
        info = device.hello()
    ssid, password = wifi_credentials(input("Wi-Fi SSID: "), getpass.getpass("Wi-Fi password (empty for open network): "))
    owner = admin.request()
    bundle = admin.request("/prepare", {"deviceId": info["candidateId"], "replaces": previous_id})
    if bundle.get("version") != 1 or bundle.get("deviceId") != info["candidateId"] or bundle.get("bridgeId") != owner.get("bridgeId"):
        raise PairingError("Invalid bridge pairing identity.")
    try:
        der = ssl.PEM_cert_to_DER_cert(bundle["certificate"])
        fingerprint = hashlib.sha256(der).hexdigest()
        if not HEX_ID.fullmatch(bundle["bridgeId"]) or not HEX_SECRET.fullmatch(bundle["secret"]):
            raise ValueError()
    except (KeyError, TypeError, ValueError):
        raise PairingError("Invalid bridge pairing certificate or credential.") from None
    print(f"Mac identity: {bundle['bridgeId']}")
    print(f"Certificate SHA-256: {fingerprint}")
    record = {"command": "pair", **bundle, "ssid": ssid, "password": password}
    device.send(record)
    device.reply({"confirm_on_device"})
    print("Confirm the Mac identity on the device. Release BOOT, then hold it for 1.5 seconds. PWR cancels.")
    try:
        device.reply({"paired"}, timeout=75)
    except PairingError:
        # Power loss or a lost serial reply after atomic NVS commit is recoverable.
        saved = device.hello()
        if saved["state"] != 1 or saved.get("deviceId") != bundle["deviceId"] or saved.get("bridgeId") != bundle["bridgeId"]:
            raise
    admin.request("/activate", {"deviceId": bundle["deviceId"]})
    print("Pairing saved and activated. The device reconnects automatically; no firmware rebuild was performed.")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB CDC port; automatically detected when one board is connected")
    parser.add_argument("--status", action="store_true", help="Read USB pairing and secure-storage status without changing the device")
    parser.add_argument("--config", type=Path, default=Path(os.environ.get("CODEX_ATTENTION_CONFIG", ROOT / "bridge/config.json")))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--reset", action="store_true", help="Physically confirm reset and revoke the old credential")
    mode.add_argument("--replace", action="store_true", help="Reset, revoke, and re-pair on the same Mac (two device confirmations)")
    args = parser.parse_args(argv)
    device = None
    try:
        port = args.port
        if port is None:
            try:
                from serial.tools.list_ports import comports
            except ImportError:
                raise PairingError("Install scripts/requirements-attention.txt to detect USB boards.") from None
            port = select_usb_port(comports())
        device = DeviceSerial(port)
        info = device.hello()
        if args.status:
            print(json.dumps({key: info.get(key) for key in ("storageReady", "state", "deviceId", "bridgeId")}))
            return 0
        if info.get("storageReady") is not True:
            raise PairingError("Secure storage unavailable. Complete the one-time owner HMAC-key provisioning in docs/attention-pairing.md; nothing was erased or burned.")
        admin = LocalAdmin(args.config)
        if args.reset:
            finish_reset(device, admin, info)
            print("Device is unpaired." if info["state"] == 0 else "Pairing reset is complete; the previous credential was revoked before clearing.")
        else:
            enroll(device, admin, info, replace=args.replace)
        return 0
    except (PairingError, OSError, KeyboardInterrupt) as error:
        print(str(error) if isinstance(error, PairingError) else "Pairing interrupted or serial connection unavailable; check device status before retrying.", file=sys.stderr)
        return 1
    finally:
        if device is not None:
            device.close()


if __name__ == "__main__":
    raise SystemExit(main())
