# Architecture

## Goal

The display is a physical inbox, not a process monitor. Its list is the union of
threads that are waiting, unread, or pinned. A selected thread can be opened to
read its latest useful text without marking it read. It also acts as a
standards-based USB microphone and a battery-capable authenticated Wi-Fi
microphone for Desktop Voice.

## Sources of truth

| Signal | Primary source | Fallback | Mutated? |
|---|---|---|---:|
| Thread metadata | `thread/list` | Last in-memory successful page | No |
| Waiting for approval | `ThreadStatus.activeFlags.waitingOnApproval` | None | No |
| Waiting for user input | `ThreadStatus.activeFlags.waitingOnUserInput` | None | No |
| Pinned | Built-in pinned thread section | Recognized persisted pin-ID keys | No |
| Unread / manually unread | Recognized IDs in `.codex-global-state.json` | None | No |
| Newly completed | Live `turn/completed` intersected with unread | Plain unread after restart | No |
| Latest text | `thread/turns/list`, full items, newest first | `thread/read(includeTurns=true)`, then the thread's local rollout file | No |
| Voice target | Last exact-ID focus accepted by the menu-bar companion | None; shown as inferred | Explicit commands only |
| Microphone privacy | ESP32 local PCM gate | Silence | Physical long press |
| Wireless microphone session | Native WSS session UUID/generation and recorder state | Idle/error summary | Physical long press and stop |

## Bridge components

### `CodexAppServerClient`

- starts `codex app-server --listen stdio://`;
- performs `initialize` → `initialized`;
- paginates `thread/list`;
- reads recent full turn items only when a detail view is requested;
- falls back to `thread/read` and then the thread's local rollout file when an
  older App Server does not support transcript APIs;
- never logs or publishes full transcripts except to the authenticated caller
  requesting one current attention thread.

### `DesktopStateReader`

- reads only `~/.codex/.codex-global-state.json`;
- recognizes current and legacy unread/pinned key shapes;
- retains the last good IDs across a transient parse failure;
- never writes the file.

### `CodexAttentionService`

- merges sources, ranks cards, and truncates the device payload;
- caches metadata and short-lived latest-text results;
- only serves detail for IDs in the current attention payload or current Voice target;
- reconnects App Server and preserves the last good list.

### `DesktopVoiceController`

- runs in the Swift menu-bar companion;
- exchanges authenticated commands with its bridge child through a private,
  per-launch, mode-0700 IPC directory;
- opens a configurable exact-thread deep link and sends the user-configured
  Codex Voice hotkey through macOS accessibility APIs;
- reports focus as inferred until Codex exposes a stable acknowledgement;
- owns the authenticated native WSS listener and Keychain-backed pairing;
- receives bounded PCM frames only after the session is armed, then hands them
  to the shared local Speech recorder; audio is never written to disk or sent
  through the Node bridge.

### HTTP bridge

- `GET /` — browser list/detail preview;
- `GET /healthz` — public health without thread content;
- `GET /api/v1/attention` — authenticated list;
- `GET /api/v1/threads/:id/latest` — authenticated, bounded latest text;
- `POST /api/v1/refresh` — authenticated forced refresh.
- `GET /api/v1/desktop/state` — authenticated Desktop/Voice reconciliation;
- `POST /api/v1/desktop/focus` — authenticated, idempotent exact-ID focus;
- `POST /api/v1/desktop/voice` — authenticated, idempotent start/resume or mute.

Desktop responses also carry the optional `wirelessMicrophone` capability and
monotonic `wirelessSession` summary. The bridge never carries the PCM stream.

## Firmware components

- `wifi_manager`: Wi-Fi connection/retry state and one-time SNTP clock setup
  required for TLS certificate validity checks.
- `attention_client`: authenticated list/detail HTTP client with a 64 KiB hard
  response ceiling.
- `attention_ui`: LVGL list, persistent selection, and scrollable detail view.
- `button_input`: debounced BOOT/GPIO0 plus AXP2101 PWR short-press polling.
- `voice_audio`: shared 48 kHz duplex I²S setup, one ES7210 reader, bounded
  capture ring, source selection, and privacy gate.
- `usb_microphone`: mono PCM16 USB Audio Class microphone for macOS.
- `wireless_microphone`: authenticated WSS client, session handshake, bounded
  PCM framing, acknowledgements, and stop/failure gating.
- `voice_control`: pure focus-before-voice state transitions.
- `main`: independent list polling, detail fetching, and button tasks.

All LVGL mutation is protected by the Waveshare BSP display lock. Detail HTTP
reads do not run in the LVGL task. A returned detail is rendered only if its ID
still matches the thread currently open, preventing stale network responses from
replacing a newer selection.

## Physical controls

```text
BOOT short  -> next highlighted item
PWR short   -> open highlighted item / return from detail
BOOT detail -> next item and open it
Touch       -> scroll; tap a card to open
Either button 1s -> start/resume or mute Voice for the selected/detail task
Fixed card tap   -> focus; tap the still-selected card again for detail
```

The AXP2101 integration enables short- and long-press IRQs and changes only REG
27 bits 5:4 to select its documented one-second IRQ threshold. It preserves the
independent OFFLEVEL bits and long-hold shutdown configuration. Physical
behavior still requires verification on the exact board.

## Failure behavior

- Codex unavailable: cached list remains and diagnostics report the error.
- Desktop state unreadable: waiting and pin signals continue; unread availability
  is shown as degraded.
- Wi-Fi down: last list remains and firmware reports connection failure.
- Detail failure: the current thread stays selected and the detail screen shows
  an error; PWR returns to the list.
- Thread removed while open: the next list refresh safely returns to the list.
- Oversized payload: bridge truncation plus firmware response/text caps bound
  memory use.
- Desktop control unavailable or wrong task: the ESP32 PCM gate remains closed.
- Wi-Fi or bridge loss while starting Voice: the PCM gate remains closed.
- Wireless certificate/credential failure: no task operation or PCM is
  accepted; USB remains available when selected.
- Wireless ingress overflow, skipped frame, stale acknowledgement, or stop
  timeout: capture closes immediately and the partial transcript remains a
  review-only draft.
