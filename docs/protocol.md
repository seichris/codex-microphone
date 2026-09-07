# Device protocol v1

All device data endpoints require:

```http
Authorization: Bearer <bridge-token>
Accept: application/json
```

Responses are `Cache-Control: no-store`. A missing or incorrect token returns
HTTP 401.

## Attention list

```http
GET /api/v1/attention
```

```json
{
  "version": 1,
  "attentionFilter": "unread+pinned",
  "generatedAt": "2026-09-01T00:00:00.000Z",
  "count": 3,
  "totalCount": 5,
  "truncated": true,
  "desktopControlAvailable": true,
  "currentThread": {
    "id": "opaque-current-thread-id",
    "title": "Desktop Voice implementation",
    "project": "codex-esp32-display",
    "status": "idle",
    "focusConfidence": "inferred",
    "voiceState": "muted"
  },
  "capabilities": {
    "desktopFocus": true,
    "desktopVoiceHotkey": true,
    "powerButtonLongPress": true,
    "wirelessMicrophone": true
  },
  "wirelessSession": {
    "sessionId": null,
    "transport": null,
    "state": "idle",
    "revision": 0,
    "errorCode": null
  },
  "items": [
    {
      "id": "opaque-thread-id",
      "title": "Implement cadence smoothing",
      "preview": "Please implement cadence smoothing…",
      "project": "open-bike-computer",
      "cwd": "/Users/chris/open-bike-computer",
      "status": "waiting_input",
      "unread": true,
      "pinned": true,
      "newResult": false,
      "updatedAt": 1788220800,
      "ageSeconds": 120,
      "reasons": ["waiting_input", "unread", "pinned"]
    }
  ],
  "diagnostics": {
    "desktopStateAvailable": true,
    "sourceError": null
  }
}
```

Firmware consumes the ID, title, project, status, age, unread, pinned, and new
flags, plus top-level count/truncation/diagnostics, current-thread state, and
Desktop-control capabilities. The current thread is omitted from `items` so it
can be rendered once in a fixed card.

Status values: `idle`, `running`, `waiting_input`, `waiting_approval`, `error`.
Running or error alone does not cause inclusion.

## Latest thread text

```http
GET /api/v1/threads/<url-encoded-thread-id>/latest
```

Only a thread present in the current bounded attention payload or identified by
`currentThread` may be read. Any other thread returns HTTP 404.

```json
{
  "version": 1,
  "id": "opaque-thread-id",
  "title": "Implement cadence smoothing",
  "project": "open-bike-computer",
  "status": "waiting_input",
  "reasons": ["waiting_input", "unread"],
  "updatedAt": 1788220800,
  "generatedAt": "2026-09-01T00:00:01.000Z",
  "kind": "agent",
  "text": "The implementation is complete…",
  "truncated": false
}
```

`kind` is one of `agent`, `plan`, `user`, `preview`, or `empty`. Text is capped
at 5,600 UTF-8 bytes by the bridge.

## Desktop focus and Voice

All commands require the same bearer token as the list endpoint. Requests must
use `Content-Type: application/json`, are limited to 4 KiB, reject unknown
fields, and use an idempotent `requestId`.

```http
POST /api/v1/desktop/focus
```

```json
{
  "threadId": "opaque-thread-id",
  "requestId": "device-focus-42"
}
```

```http
POST /api/v1/desktop/voice
```

```json
{
  "threadId": "opaque-thread-id",
  "requestId": "device-voice-43",
  "command": "start-or-resume"
}
```

`command` is `start-or-resume` or `mute`. Query reconciliation state with:

```http
GET /api/v1/desktop/state
```

Successful responses include the exact acknowledged `threadId`,
`focusConfidence`, `voiceState`, `desktopControlAvailable`, and capabilities. The bridge returns HTTP 503
with `desktop_control_unavailable` when it is not running as a child of the
menu-bar companion. Starting Voice requires a prior focus acknowledgement for
the same thread ID.

`desktopVoiceHotkey` describes the legacy HTTP/USB start path. It remains false
when only the WSS microphone is ready, so older firmware cannot mistake a
wireless-only companion for a usable USB recorder. `wirelessMicrophone` and
`wirelessSession` are additive v1 fields; consumers must tolerate their absence
from older companions.

`focusConfidence` is `confirmed`, `inferred`, or `unavailable`. The current
implementation reports `inferred`: opening the exact-ID deep link succeeded,
but Codex Desktop does not expose a public selected-task acknowledgement.

`desktopControlAvailable` reports a successful private companion IPC response,
independently of `diagnostics.desktopStateAvailable` (the unread/pinned file).
It is false when the controller is absent or fails to respond. A missing field
in an older bridge response means unknown availability.

A null `currentThread` means the bridge has no observed or device-requested task
ID. It does **not** mean that no task is open on the Mac. With a healthy
controller, the firmware shows `MAC TASK UNKNOWN` and offers the existing
list-selection/long-press flow. After an exact-ID focus request it shows
`VOICE TARGET` for inferred focus; only confirmed evidence allows `CURRENT ON
MAC`. Recent, pinned and unread IDs are never used to guess Desktop selection.

## Wireless microphone

The wireless transport is a separate authenticated WebSocket application
protocol, `codex-microphone.v1`, served by the native macOS companion on port
5181 by default. It is not routed through the bridge's HTTP bearer-token
endpoints. The board connects with `wss://`, validates the provisioned
certificate/host, and sends its per-board credential in the first application
message. The companion rejects unauthenticated operations and closes the
connection after three seconds without `hello`.

Control messages are bounded UTF-8 JSON objects (maximum 4 KiB) and always carry
`version: 1`. The handshake is:

```text
board hello(deviceID, credential)
companion capabilities(format=48 kHz mono PCM16, maxFrameBytes=1956)
board start(requestID, threadID, transport=wifi)
companion prepared(sessionID, generation, format)
board commit(sessionID, generation)
companion armed(sessionID, generation)
board -> binary PCM frames
companion listening(first sequence), then ack(every five frames)
board stop(finalSequence)
companion stopped(finalSequence)
```

After `prepared`, every session message carries the server-issued UUID and
connection generation. Request IDs are idempotent only for the same task and
transport; conflicting reuse fails. Reconnects create a new session and cannot
replay earlier frames.
The companion accepts one board connection and one active session at a time.

Each binary message is exactly 1,956 bytes: a 36-byte network-order header
(`CMIC`, framing version, flags, header length, session UUID, 32-bit sequence,
and 64-bit first-sample index) followed by 1,920 bytes of signed little-endian
PCM16. Sequence and sample index start at zero. Unknown flags, malformed
headers, skipped/duplicate sequences, stale sessions, oversized messages, and
missing stop acknowledgements fail closed. No silence is synthesized for a
missing frame.

## Versioning

Firmware rejects responses whose `version` is not `1`. Additive fields are
allowed within v1. Removing or changing a field's meaning/type requires a new
protocol version.
