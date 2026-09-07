# Wireless capture ownership and qualification

Implementation follows the physical `6fc7a83` trace that reached Mac commit/armed
but had zero validated PCM. That trace does not prove whether the board received
armed, and this change does not claim a physically fixed device.

## Firmware contract

The long-press handler revokes capture before display/queue/network waits. A new
start retains the returned closed capture token through focus and preparation.
`voice_audio_start_capture(source, token)` selects the source, clears old queued
samples, and opens only that authorization. It returns errors; it cannot adopt
a newer epoch after cancellation. Cleanup uses `voice_audio_stop_capture(token)`
and cannot revoke a successor. USB host mute does not control Wi-Fi capture.

The 25-frame PCM payload (48,000 bytes) is explicitly in PSRAM; the queue control
block stays internal. Wireless recording readiness includes codec/resources and
capture-task creation. Creation alone is not proof of scheduling or codec reads.

Wireless phase is authoritative: idle -> preparing -> wait-armed -> opening-capture
-> streaming -> stopping -> idle. Failure/cancellation are terminal for that take.
Outstanding start/stop/send/recovery operations prevent premature reuse. UI notices
are consumable separately from terminal outcomes and identify their capture token.
Legacy HTTP polling is allowed to reconcile USB only, not the WSS owner.

State locks protect memory only: no blocking capture, network, power-policy calls,
or second mutex under the wireless state lock. The stream waits for PCM without
holding its send mutex. It reserves a complete frame at submission; Stop revokes
capture, then serializes with that reserved send before choosing the exclusive
`finalSequence`. An already submitted frame cannot be withdrawn from TCP.

Callbacks do not send control messages or stop/restart the WebSocket client.
The connection supervisor sends deferred hello/commit and retires failed sessions.
An uncertain/partial write poisons the connection: no packet replay and no cancel
packet appended into that byte stream. A timeout before a server UUID exists also
retires the connection, rather than leaving cancellation latched indefinitely.
Reconnect restores readiness only; recording requires a new physical gesture.

The supervisor checks progress even when the audio gate is closed or the sender
has no progress. Existing 5 s startup, 1 s capture/ACK, 250 ms send and 250 ms stop
budgets are not increased. A library timeout is per operation/wait, not a proven
bound for the whole TLS/WebSocket call or its internal cleanup. Scheduling and
library blocking still require measurement on the board.

Power-save restoration belongs to terminal cleanup, including preparation cancel,
recording failure, normal stop and duration expiry. The supervisor uses event-driven
idle waits rather than adding a permanent 50 Hz idle loop. The codec still reads
while gated: codec power-down is NOT implemented without hardware wake-up testing.
No measured battery-life or idle-current improvement is claimed.

## Diagnose the next physical run

Keep library payload dumps disabled. The bounded `mic a=... stage=...` transition
and pre-failure snapshots contain no audio, text, task identity or credential.
Compare one attempt's snapshots, not lifetime counter totals across boots.

| Field/stage | Interpretation |
| --- | --- |
| `a`, `phase`, `token`, `epoch`, `src`, `ready` | Attempt and capture ownership/resources. Gate is epoch bit zero. |
| `arm`, `reject` | Complete armed-handler entries and rejected transitions. Mac local send completion is not this observation. |
| `gate-open`, `gate-lock-timeout`, `gate-unavailable-or-revoked` | Explicit activation outcome, no silent successful-looking start. |
| `loop`, `age`, `read` | Sender loop count, last-progress age and read-attempt count. No progress does not alone distinguish Ready starvation from a blocked task. |
| `send=completed/attempted`, `lock`, `seq` | Submission boundaries, send-lock failures and exclusive next sequence. |
| `codec=completed/started`, `err` | Codec activity independent of gate/consumer progress. |
| `q=dequeued/queued`, `drop`, `open=failed/attempted`, `close` | Capture pipeline deltas and revocations. |

No armed entry locates failure before the application handler, not specifically
a lost radio packet. Armed + gate failure locates activation. Gate open + no codec
completion locates capture/driver blocking. Queued frames + no sender progress
locates consumption/scheduling/lock boundaries. Send entry without validated Mac
frame locates transport/receive/validation. Use task-state/runtime tracing to split
Ready-without-runtime starvation from Blocked-on-lock; neither is inferred from
priority numbers alone. Current counters do not include a full per-caller close
event ring, socket-level poll location, or RTOS runtime trace.

Mac logs distinguish first validated frame, recorder admission, first Speech
append, and recorder drain. `contentProcessed` remains explicitly local processing.
Test processes use `dictation-tests.log`; app processes use `dictation.log` with
origin, run identifier and monotonic event time. Never treat test stages as a
physical recording. ACK means bounded recorder-queue admission, not recognition
success; `stopped` means admitted frames drained and endAudio executed, not a
final transcript/composer receipt. Duplicate Stop shares the same drain barrier.

## Automated checks

```
python firmware/tests/run_voice_audio_tests.py
python firmware/tests/run_wireless_lifecycle_tests.py
swift test --package-path macos
swift build --package-path macos --configuration release
```

The C harness includes production capture, wireless lifecycle and binary framing
together. RTOS, codec, cJSON parsing, TLS and socket behavior are substitutes, so
it establishes deterministic ownership/error handling only. It exercises stale
activation, queue-lock failure, real gate/framing integration, cancel-before-UUID,
callback reentrancy, uncertain send, lost stop ACK and bounded control assembly.

`WirelessAudioIngressTests` exercises the actual bounded admission helper and
concurrent close/drain ordering. `WirelessNativeTransportTests` creates real
Network.framework TLS/WebSocket connections to the production server using a
synthetic identity in a temporary keychain and synthetic PCM. It checks actual
handshake, validated framing, sink rejection, drain/duplicate-stop ordering,
session reuse, stale cancellation, hostname verification and reconnect. It does
not use the production pairing/keychain or require Speech/microphone permissions.
It is NOT proof of ESP WebSocket interoperability, radio/codec capture or Speech.
Run the real board against the same production listener for that boundary.

Before release, use pinned esp_websocket_client 1.8.0 and record the exact IDF,
component resolution, sdkconfig, firmware/app SHA and board identity. Run both
IDF 5.4.4 and 5.5 builds. Do not flash from CI or commit provisioning secrets.

## Physical acceptance (not yet performed by this implementation)

| Scenario | Required evidence |
| --- | --- |
| Battery cold boots; 50 warm Wi-Fi-only starts | Capture ready, board armed/gate open, valid first PCM admitted and appended. No USB data connection influencing AUTO choice. |
| Repeated 45-50 s recordings | Contiguous sequences/sample accounting, bounded queues/ACK age, no unexplained writes; separately verify Mac 55 s and board 60 s limits. |
| Second press, either button | Gate closes before UI/network waits; final sequence accounts for reserved transmission; original task receives text once and remains unsent. |
| Select B, hide A by filtering, or open Settings while recording A | Stop and handoff remain bound to A. |
| Stop during focus/prepared/armed/first send | Revoked authorization never reopens; cleanup cannot mute the successor. |
| Wi-Fi loss, Mac restart/sleep, lost stop ACK | Immediate local revocation when detected; bounded recovery; no automatic capture, frame replay or transcript replay. |
| USB hotplug, host mute and active UAC polling during Wi-Fi take | USB remains silent, no source takeover or Wi-Fi suppression. |
| Idle after success/failure/early cancel/expiry | Saved modem policy restored; measure actual battery current for idle and repeated recording. Do not infer system idle power from Wi-Fi policy. |

## Task selection and handoff limits

Retained device selection is an explicit target, not proven foreground selection.
A recording's exact task remains immutable. Subscription-following events still
do not establish an authoritative active window/pane/thread source.

The draft-link open request is not a composer-delivery acknowledgement. One
memory-only recovery copy retains original task and text after a handoff attempt;
it is explicitly clearable and replaced by the next handoff. No automatic retry,
send, transcript logging or disk persistence is introduced. A lost stop ACK or
uncertain link result must not trigger blind repeat insertion. Reliable delivered
status still requires a Codex exact-task composer acknowledgement with idempotency.
