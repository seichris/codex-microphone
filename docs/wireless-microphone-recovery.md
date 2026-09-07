# Wireless microphone provisioning and recovery

The thread list and microphone use separate connections. The list uses the Mac
bridge on HTTP port 5180 with its bridge token. Audio uses the paired TLS
WebSocket listener on port 5181 with a different board credential. A reachable
board, an open port, or a green bridge status does not establish audio readiness.

## Provisioning

Generate a private pairing bundle with `scripts/create-wireless-pairing-bundle.sh`.
Use a DHCP reservation or a stable DNS name for the Mac. DNS names and IPv4
addresses are supported by these scripts. `--server-name` specifies the identity
validated by TLS; the generated certificate SAN matches that name even when the
connection host differs. Import the same bundle into the Mac companion.

Run `scripts/provision-wireless-microphone.sh <bundle.json> [firmware-directory]`.
The script requires `jq` and Python 3. It creates mode-600 wireless defaults and
updates only pairing/transport/TLS-date settings in an existing `sdkconfig`,
preserving Wi-Fi and bridge configuration. This matters because IDF defaults do
not overwrite values already present in `sdkconfig`. A re-pair requires both a
Mac import and a new firmware build/flash.

Keep `sdkconfig`, its backups, generated defaults, pairing bundles, and firmware
binaries private. Firmware contains Wi-Fi, bridge, and pairing credentials.
The repository ignores its standard generated paths; arbitrary bundle names or
output directories require their own ignore rules.

Kconfig cannot represent physical newlines in a string setting. Provisioning
preserves escaped line breaks; firmware decodes them exactly once and validates
the resulting PEM before starting the WebSocket client. Do not paste a PEM into
`sdkconfig` or disable certificate verification to bypass provisioning failures.
The companion selects its identity by exact certificate bytes, since Keychain
labels can refer to multiple certificates after a rotation.

TLS date verification is enabled. On a cold battery boot, wireless startup waits
for Wi-Fi and a usable clock from SNTP (`pool.ntp.org`). If UDP 123 or DNS is
blocked, fix that network access; the firmware does not bypass date validation.
Wi-Fi retries use delays from 1 to 30 seconds, reset after DHCP succeeds, and log
numeric disconnect reasons without printing network credentials. Loss of the
DHCP address clears connection readiness.

TLS allocations use PSRAM on this board. Its 16 KiB receive buffer and handshake
working memory must not compete with USB, Wi-Fi, audio and DMA for internal RAM.
Provisioning updates the allocation choice in existing sdkconfig files too;
defaults alone do not replace a previously selected internal-only allocator.
Certificate, hostname and date validation remain enabled.

Open Settings on the board to check "Wi-Fi mic" status. "Connected and paired"
means the board has authenticated its WebSocket connection, not merely joined
Wi-Fi. A clock-wait status identifies SNTP startup; failed connections retain
numeric TLS, ESP and HTTP error codes through subsequent disconnect events.
The status includes no credentials or certificate material.

A second long press stops the active recording using its retained task ID,
including when its card disappears after becoming read, another card is selected,
or Settings is open. The screen selection is consulted only to start a new
recording. Stopping closes the capture gate before waiting for network work;
the Mac then finalizes transcription and opens the original task's draft.

## Diagnosis

- An unchanged initial "Connecting to the Mac" card means no poll has rendered
  yet; it is not evidence of an empty thread filter. The startup screen now names
  the current initialization stage and reports buffer/worker allocation failures.
  Network worker stacks and large snapshot/detail buffers use PSRAM, preserving
  internal memory for Wi-Fi, TLS, USB, and DMA. Serial startup logs include free
  internal memory and the largest available block, without configuration secrets.
- Check that the board and Mac are on mutually reachable LANs. The board's IP
  and the Mac endpoint are different settings. Guest/client isolation prevents
  the board reaching either listener even when both devices have internet.
- Check `/healthz` on 5180, then the authenticated attention endpoint. Health
  alone does not prove that the board token works. An HTTP 401 is now reported
  explicitly on the device. The red dot describes bridge/Desktop data health.
- Compare the listener certificate with the imported pairing and built firmware
  configuration. Inspect only fingerprints; never print PEM, tokens, credentials,
  or Keychain private material. The companion's listener-ready status does not
  prove a paired board has connected or that Speech recognition is ready.
- Verify Speech permission, on-device recognition availability, selected task,
  and matching transport preferences. Firmware Auto prefers USB while a host is
  actively requesting samples; idle USB attachment alone is not this signal.
  Wi-Fi-only never falls back to USB. Stop/cancel must still close capture before
  transport waits, and a disconnected session requires a new physical gesture.

## USB recovery

This board uses its native ESP32-S3 USB port. The application can enumerate as
**Waveshare Voice Microphone** without exposing a serial port. That proves USB
audio enumeration, not ROM download mode. Inspect `ioreg -p IOUSB -w 0` and
`/dev/cu.*`; do not diagnose missing hardware solely from absent `usbmodem` paths.

To enter download mode, GPIO0/BOOT must be held low across reset or power-on.
With a battery attached, unplugging USB alone does not necessarily reset the
board. Fully power the board off, then hold BOOT while powering it on, or hold
BOOT across a real reset. Use a known data cable directly connected to the Mac.
Release BOOT after the ESP32 download device appears. If it still does not
enumerate, recheck power, cable, and actual button behavior before another flash
attempt; host software cannot force a non-serial audio device into the ROM loader.

Once a serial port appears, confirm the physical board and ESP32 identity before
flashing. Use the build's generated flash arguments, and require esptool write
verification plus an explicit `verify_flash`/hash check for the written images.
A successful build, a ping reply, or audio-device enumeration is not deployment
evidence. After flashing, test USB, cold battery startup, list loading, recording,
stop/cancel, and Wi-Fi interruption/recovery on the actual board.

References: [Espressif boot-mode selection](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/boot-mode-selection.html)
and [Waveshare board documentation](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06).

## Focused regression checks

Run `python scripts/test-wireless-provisioning.py` using the ESP-IDF Python
environment (or install `esp-idf-kconfig==2.5.4`). It generates synthetic pairing
material and verifies the path through real Kconfig, C compilation, and PEM
decoding, plus rotation and certificate naming. No production secrets are used.
Firmware host tests include Wi-Fi reconnect/DHCP behavior and TLS startup gating.
The UI test covers visible startup failures. The firmware compiler rejects main
worker stack frames above 2 KiB, including builds with the maximum 40 cards;
snapshot and detail sizes must not consume the workers' call-stack budget.
Run the bridge suite, Swift tests/release build, and the ESP-IDF firmware build
before deploying changes. Host fixtures do not establish physical radio timing,
codec behavior, or battery performance.
