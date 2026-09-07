# Codex ESP32 Display

A physical **Codex attention inbox** for the rectangular
[Waveshare ESP32-S3-Touch-AMOLED-2.06](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06)
(410×502 AMOLED, touch, BOOT button, and PWR button).

It intentionally does not show every historical thread. By default it shows
only threads that are both unread and pinned, so the device stays focused on
the threads you explicitly chose to keep visible.

The previous attention inbox, which included waiting, unread, or pinned
threads, remains available with the `all` filter. A thread appears once even
when several reasons apply.

## Controls

- **BOOT short press:** highlight the next thread, wrapping at the end.
- **PWR short press:** open the highlighted thread's latest text.
- **Either button, held for one second:** focus the selected/detail thread and
  start device dictation over USB or paired Wi-Fi; hold again to finish
  transcription. The Mac companion opens the unsent text in that exact task's
  composer.
- **PWR on the text screen:** return to the inbox.
- **BOOT on the text screen:** jump to the next thread and load its latest text.
- **Touch:** scroll the inbox or text screen; tap a thread card to open it;
  tap the fixed bottom Voice Target card once to select/focus it and again to
  open its details; tap the gear to adjust title and subtitle sizes, then use
  the back arrow to return to the inbox.

The firmware observes the AXP2101 one-second long-press interrupt without
changing its separate four-to-ten-second hardware shutdown threshold.

## What is implemented

### Mac bridge

- launches `codex app-server` over JSONL stdio;
- reads thread names, projects, timestamps, live status, and pinned-section data;
- reads Codex Desktop's local unread-ID set **read-only**;
- observes `turn/completed` and status notifications;
- exposes a bearer-token-protected attention-list endpoint;
- exposes an on-demand latest-text endpoint for threads currently in the inbox;
- exposes authenticated Desktop state/focus/Voice endpoints backed by a private
  per-launch channel to the native companion;
- includes a browser dashboard with the same list/detail interaction;
- reconnects when App Server exits and preserves the last good list.

The bridge has no runtime npm dependencies.

### ESP-IDF firmware

- targets the Waveshare ESP32-S3-Touch-AMOLED-2.06 only;
- uses Waveshare's managed BSP and LVGL 9.5;
- renders a touch-scrollable 410×502 inbox;
- redraws complete screens through reserved DMA strips, with checked transfers
  and retries so failed updates are not silently accepted;
- renders a fixed bottom card for the current or targeted Desktop Voice thread;
- preserves the selected thread across list refreshes;
- reads BOOT on GPIO0 and the PWR short/long-press latches from the AXP2101 over the
  BSP's shared I²C bus;
- fetches long thread text in a separate FreeRTOS task, so the UI remains usable;
- discards stale detail responses after the user changes threads;
- keeps the last good list when Wi-Fi or the bridge temporarily fails.
- plays a short two-tone chime once when a new attention item or attention reason
  appears; audio initialization failure leaves the rest of the device usable.
- enumerates as a 48 kHz mono USB microphone and sends silence unless the local
  Voice state is explicitly listening; stops on a fresh non-listening companion
  snapshot and enforces a 60-second limit.
- supports an authenticated WSS microphone transport for battery-only use, with
  fixed 20 ms PCM frames, bounded queues, explicit session acknowledgements,
  and Auto/USB/Wi-Fi source selection. USB and Wi-Fi never share a live session.

## Architecture

```text
Codex Desktop state                 codex app-server
~/.codex/.codex-global-state.json  JSONL over stdio
          │ unread IDs                    │ thread metadata/status/turns
          └──────────────┬─────────────────┘
                         ▼
                 Mac bridge :5180
      attention/detail + authenticated Desktop control endpoints
                         │ private per-launch IPC
                         ▼
              native menu-bar companion
            thread deep link + Voice hotkey
                         │ Bearer token / LAN
                         ▼
         Waveshare ESP32-S3-Touch-AMOLED-2.06
   touch + BOOT/PWR + fail-closed USB microphone stream
```

The bridge never edits Codex thread data. Opening text on the ESP32 does **not**
mark a thread read in Codex Desktop. A focus request may navigate Codex
Desktop, and the Voice request invokes its configured keyboard shortcut.

## 1. Start the Mac bridge

Prerequisites:

- macOS with Codex Desktop or Codex CLI installed;
- Codex CLI on `PATH`, or bundled inside `ChatGPT.app`/legacy `Codex.app`;
- Node.js 18.18 or newer.

```bash
git clone https://github.com/seichris/codex-esp32-display.git
cd codex-esp32-display/bridge
npm run setup
npm start
```

`npm run setup` creates `bridge/config.json` with a random 256-bit bearer token.
It refuses to overwrite an existing config.

Open the browser dashboard:

```text
http://127.0.0.1:5180/
```

The ESP32 list endpoint is:

```text
http://<mac-lan-ip>:5180/api/v1/attention
```

A typical macOS Wi-Fi address can be printed with:

```bash
ipconfig getifaddr en0
```

Allow incoming Node connections if the macOS firewall asks.

### Bridge configuration

`bridge/config.json`:

```json
{
  "host": "0.0.0.0",
  "port": 5180,
  "token": "generated-by-npm-run-setup",
  "pollIntervalMs": 2000,
  "maxThreads": 300,
  "maxItems": 30,
  "attentionFilter": "unread+pinned",
  "codexBin": "codex",
  "codexHome": "~/.codex"
}
```

Environment overrides are also supported:
`CODEX_ATTENTION_CONFIG`, `CODEX_ATTENTION_HOST`,
`CODEX_ATTENTION_PORT`, `CODEX_ATTENTION_TOKEN`,
`CODEX_ATTENTION_POLL_MS`, `CODEX_ATTENTION_FILTER`, `CODEX_BIN`, and
`CODEX_HOME`. The filter defaults to `unread+pinned`; set it to `all` to
restore the waiting/unread/pinned attention inbox.

### Run at login

```bash
./scripts/install-macos-launch-agent.sh
```

Logs are written under `~/Library/Logs/CodexESP32Display/`. Remove the service
with:

```bash
./scripts/uninstall-macos-launch-agent.sh
```

## 2. Flash the Waveshare board

Install and export ESP-IDF 5.4 or newer, then:

```bash
cd firmware
idf.py set-target esp32s3
idf.py menuconfig
```

Under **Codex ESP32 Display**, set:

1. Wi-Fi SSID and password;
2. the full attention endpoint printed during bridge setup;
3. the same bearer token.

For battery-only dictation, generate and import a local pairing bundle in the
Mac companion, then provision its non-secret endpoint plus certificate and
credential into the firmware build configuration:

```bash
scripts/create-wireless-pairing-bundle.sh \
  --host <mac-lan-ip-or-dns-name> \
  --output "$HOME/wireless-pairing-CESP32VOICE01.json"
scripts/provision-wireless-microphone.sh \
  "$HOME/wireless-pairing-CESP32VOICE01.json" firmware
```

See [macOS pairing instructions](macos/README.md#pairing-the-wi-fi-microphone)
for the listener import and `SDKCONFIG_DEFAULTS` build command. Pairing files
contain private keys and board credentials, are mode 600, and are ignored by
Git.

Then:

```bash
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

The first build downloads the official
`waveshare/esp32_s3_touch_amoled_2_06` BSP and LVGL through the ESP-IDF
Component Manager.

## 3. Optional macOS menu-bar companion

The native SwiftUI companion owns the bridge process and shows a small outline
of the 2.06 device in the macOS menu bar:

```bash
./macos/build_app.sh
open "macos/build/Codex ESP32 Display.app"
```

It replaces the bridge LaunchAgent while running and provides bridge status,
start/stop, the dashboard, endpoint copying, Voice status/settings, and log
access from its menu. Opening the dashboard from the menu automatically seeds
its token and clears the URL fragment after the dashboard stores it locally.
Speech Recognition permission is required for native dictation. USB mode also
needs Mac Microphone permission; Wi-Fi mode receives the board's WSS PCM
directly and does not enumerate a Mac capture device. Audio is transcribed
locally and is never saved.

## Attention rules

```text
unread
AND pinned
```

The default `unread+pinned` filter requires both flags. Set
`attentionFilter` to `all` (or set `CODEX_ATTENTION_FILTER=all`) to use the
previous attention inbox:

```text
waiting_for_approval
OR waiting_for_user_input
OR unread
OR pinned
```

With the `all` filter, priority is:

1. waiting for approval;
2. waiting for user input;
3. newly completed and unread while the bridge was running;
4. any other unread thread;
5. pinned thread;
6. newest update first within a group.

After a bridge restart, an unread result still appears as **UNREAD**, but only a
completion observed live by that bridge process gets the **NEW** badge. The
persisted unread set does not record why a thread became unread.

## API

List:

```bash
curl -H "Authorization: Bearer <token>" \
  http://127.0.0.1:5180/api/v1/attention
```

Latest text for a thread currently in that list:

```bash
curl -H "Authorization: Bearer <token>" \
  http://127.0.0.1:5180/api/v1/threads/<thread-id>/latest
```

Desktop state and commands use the same bearer token:

```text
GET  /api/v1/desktop/state
POST /api/v1/desktop/focus
POST /api/v1/desktop/voice
```

The latest-text response prefers the newest Codex agent message, then a plan,
then a user message, then the thread preview. Text is capped at 5,600 UTF-8 bytes
for predictable ESP32 memory use.

See [docs/protocol.md](docs/protocol.md) and
[docs/architecture.md](docs/architecture.md).

## Security and compatibility

- HTTP plus a bearer token prevents accidental access to the legacy bridge; it
  does not encrypt LAN traffic. The wireless microphone uses a separate TLS
  WebSocket with a per-board credential and certificate/host verification.
- The bridge-to-companion controller uses a random token and a private
  per-launch temporary directory. Voice commands are idempotent by request ID.
- Unread state comes from Codex Desktop's internal
  `.codex-global-state.json`. Parsing is read-only and failure-tolerant, but the
  format can change.
- Current pinned sections, detailed status, and paginated turns depend on Codex
  App Server APIs. For latest text, the bridge falls back from paginated turns
  to `thread/read` and then to the authenticated thread's local rollout file
  when an older App Server exposes metadata but not transcript APIs.
- The PWR input path is implemented against the AXP2101 short/long-press status
  latches; its one-second long-press behavior, USB microphone enumeration/audio,
  the Codex Desktop thread deep link, and Voice shortcut behavior must still be
  verified on the exact hardware and installed Desktop build before treating the
  feature as production-ready.

## Development

```bash
cd bridge
npm ci
npm test
npm run check
```

GitHub Actions runs the bridge suite and an ESP-IDF 5.4.4 firmware build.

## License

MIT. See [NOTICE](NOTICE) for trademark and affiliation information.
