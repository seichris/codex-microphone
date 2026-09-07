# Wireless microphone implementation plan

Status: implemented on branch `wireless-microphone`; physical flashing and
battery/network qualification remain outstanding.

The implementation preserves this document's protocol, pairing, buffering,
and failure-boundary decisions. Automated Swift, bridge, shell, and portable C
protocol checks pass in this branch. The ESP-IDF build and exact-board speech
acceptance still require the local ESP-IDF toolchain, generated pairing
material, and an explicitly authorized device test.

## Baseline checked

Checked GitHub `main` on 2026-09-06 using `git fetch origin main` and
`git ls-remote origin refs/heads/main`. Both resolved to
[`deb13cfe817f77146dafcf0301fa6275ef8f3338`](https://github.com/seichris/codex-esp32-display/tree/deb13cfe817f77146dafcf0301fa6275ef8f3338)
(merge of PR #6, dictation accumulation). This plan's branch,
`wireless-microphone-plan`, starts directly at that commit.

The baseline has USB microphone capture, on-device English transcription on
macOS, a recording overlay, and automatic insertion into the selected Codex
task's composer. It has no wireless microphone transport. The older
[Desktop Voice plan](desktop-voice-implementation-plan.md) explicitly targeted
USB and includes obsolete hotkey/review behavior; current Swift source and
[macOS documentation](../macos/README.md) govern the integration below.

| Existing code | Consequence for this feature |
| --- | --- |
| `firmware/main/voice_audio.c` | Reads ES7210 PCM at 48 kHz, mono, signed 16-bit; currently applies both local and USB-host mute to the same read. |
| `firmware/main/usb_microphone.c` | Its USB callback is the current consumer of `voice_audio_read()`. There is no network audio producer. |
| `firmware/main/attention_audio.c` | Speaker chimes share the BSP audio infrastructure and use 48 kHz. Preserve codec/clock ownership. |
| `firmware/main/main.c`, `voice_control.c` | A long press focuses a task, waits 400 ms, starts USB dictation, then opens the PCM gate. Attention polls can stop recording. |
| `macos/Sources/CodexESP32Display/DictationRecorder.swift` | Combines USB capture and Speech recognition; startup requires the exact Waveshare USB device and its first sample buffer. |
| `DictationModel.swift`, `DictationSession.swift` | Own the selected task, recording lifecycle, partial text, overlay, and composer handoff. Reuse these semantics. |
| `DesktopVoiceController.swift`, `bridge/src/service.mjs` | Expose voice state through private file IPC and HTTP. Observed task selection can become unavailable when multiple task views exist. |
| `bridge/src/http-server.mjs` | Accepts bounded JSON commands, with no audio endpoint. Its existing bearer-authenticated HTTP transport is unencrypted. |

Local microphone-handoff edits on `fix-microphone-handoff` are outside this
baseline. Reassess their task-observation changes during implementation; this
plan does not assume they have merged or silently include them.

## User outcome and scope

With the board running on battery and connected to the Mac's LAN, holding
either button for one second starts dictation into the selected local Codex
task. Holding again stops audio immediately and finishes transcription. The
completed text opens in that task's composer using the existing handoff;
messages are never sent automatically. USB is optional for subsequent use
after setup and firmware installation.

Preserve USB dictation and add `Auto`, `USB`, and `Wi-Fi` transport preferences.
Auto chooses available USB first, otherwise paired Wi-Fi, at session start.
Lock the choice until that session ends. Disconnecting USB during a USB
recording ends that recording; the next physical start can use Wi-Fi. Plugging
USB into a Wi-Fi recording does not reroute or duplicate audio.

Initial scope is one paired board and one active dictation session per Mac,
English local transcription, and the existing duration limits. Bluetooth,
internet relays, a system-wide virtual microphone, audio playback over Wi-Fi,
and seamless transport switching during speech are outside this change.

## Architecture decision

Use a persistent, authenticated WebSocket over TLS directly from the ESP32 to
the native companion. Carry wireless session control and binary PCM on that
connection. Keep the Node bridge responsible for the existing attention list,
details, and legacy USB controls. Publish wireless session summaries back to
the bridge through the existing private IPC; never send audio through its
JSON/filesystem request queue.

```text
ES7210 -> one firmware capture task -> selected transport
                                      | USB Audio Class -> USB capture adapter --+
                                      | WSS PCM -> native wireless receiver -----+
                                                                                |
                                                          shared Speech engine <-+
                                                                                |
                                                       DictationSession / overlay
                                                                                |
                                                        exact-task composer draft
```

The proposed Mac server uses Network.framework (`NWListener`, TLS,
`NWProtocolWebSocket`), keeping audio in the process that owns recognition.
The board uses Espressif's managed `esp_websocket_client`. Verify their
interoperability on the supported macOS 13 baseline and ESP-IDF 5.4.4 before
building the complete flow. Pin the verified component release and lockfile;
do not select an untested latest release implicitly.

Start with the existing 48 kHz mono PCM16 format: 96,000 audio bytes/second,
about 0.768 Mbit/second before framing/TLS. This avoids changing the shared
codec clock or introducing a resampler alongside the transport work. Compression
or 16 kHz conversion is a later measured optimization if bandwidth, power, or
memory results require it. TCP can stall behind lost packets; bounded queues
and explicit timeouts must prevent delayed speech from accumulating.

## Pairing, identity, and readiness

1. Add companion setup that creates a local TLS identity and a separate random
   256-bit wireless credential per board. Store the private identity and secret
   in macOS Keychain; keep only nonsensitive settings in preferences.
2. Export an explicit pairing bundle with the Mac address, configurable port
   (default 5181), certificate trust material, expected server name, protocol
   version, board ID, and wireless credential. Provision it through the local
   firmware setup/build workflow into device NVS. Support initial provisioning
   in bootloader mode; do not assume the USB microphone exposes a serial port.
   Generated pairing files are ignored, permission-restricted, and excluded
   from logs, commits, and application bundles.
3. The board verifies the configured certificate chain and hostname before
   sending its credential. Provision a stable local hostname or correct IP SAN;
   never disable name verification to accommodate DHCP. Re-pair explicitly when
   identity changes. Establish valid device time before certificate validation;
   clock/certificate failures produce a specific setup error.
4. Authenticate in the first WSS application message, within a three-second
   deadline. Permit no task operation or audio before authentication. Limit
   unauthenticated connections, message size, and attempts. The legacy HTTP
   bearer token does not authorize wireless microphone sessions.
5. Discovery may suggest an address, but cannot change the provisioned identity
   or trust root. Revoke/rotate pairing credentials from Voice Settings; revocation
   closes the session and requires a fresh physical start after re-pairing.

Add actionable setup for incoming connections, firewall/local-network permission
where required by macOS, and port conflicts. USB readiness continues to require
Microphone permission and the USB device. Wi-Fi readiness requires a paired live
connection, Speech permission, and local recognition availability; it must not
depend on `AVCaptureDevice` enumeration or macOS Microphone permission for USB
capture. Verify the permission distinction on a clean macOS profile.

## Session protocol and startup ordering

Define `codex-microphone.v1` as a separate WSS application protocol. Keep legacy
HTTP protocol v1 intact. Require an application-level version exchange even
when a WebSocket subprotocol is requested. Specify strict schemas and shared
golden wire fixtures before implementing either endpoint.

Control messages include `hello`, `capabilities`, `start`, `prepared`,
`commit`, `armed`, `listening`, `ack`, `stop`, `stopped`, `cancel`, and `error`.
All session messages carry a server-issued random session ID and a connection
generation. `start` carries a unique request ID and exact local task ID. The
server binds the session to the authenticated board, connection, task, format,
and selected transport. Repeated request IDs return the same result; conflicting
reuse fails. Reconnects cannot resume an earlier session or replay its audio.

The ordering below avoids a startup deadlock: today's USB recorder waits for a
buffer before acknowledging start, while the firmware waits for acknowledgement
before releasing live audio.

1. **Idle:** maintain the authenticated connection and heartbeats; transmit no
   microphone PCM. On a physical long press, snapshot the selected task and send
   `start`; show amber “Starting Wi-Fi mic”.
2. **Prepare:** the Mac acquires the common session lock, opens the exact task,
   checks recognition readiness, and allocates bounded audio buffers. It returns
   `prepared` with session identity, format, deadlines, and limits. This says the
   receiver is prepared, not that speech is arriving.
3. **Commit:** firmware checks that the original gesture is still active, its
   target is unchanged, and its microphone is ready, then sends `commit`.
   The Mac returns `armed` only for the current prepared session.
4. **First frame:** after matching `armed`, firmware opens the selected audio
   gate and shows a persistent “Mic active — connecting” indicator immediately.
   The Mac appends the first correctly formed microphone frame to Speech and
   returns `listening`. Only then show green “Listening · Wi-Fi”. Digital silence
   is valid audio; audible speech must not be necessary for this acknowledgement.
5. **Active:** the Mac acknowledges the highest accepted frame sequence every
   100 ms. Firmware limits unacknowledged audio and watches acknowledgement age.
   The session stream, rather than an unrelated attention poll, determines
   wireless capture liveness.
6. **Normal stop:** the second long press closes capture locally first, discards
   any incomplete frame, and sends `stop` after preceding complete frames on the
   ordered stream, declaring the final sequence. Drain for at most 250 ms;
   if completion cannot be acknowledged, cancel rather than deliver a hidden
   backlog. The Mac confirms the sequence, calls `endAudio()` once, and returns
   `stopped`. Final recognition and draft insertion follow existing behavior.
7. **Failure/cancel:** close the gate, invalidate the generation, discard queued
   audio, and preserve any partial transcript for review. Interrupted sessions
   do not automatically insert partial text into the composer. Reconnection
   restores readiness only; another long press is required to record.

Use monotonic clocks for deadlines. Proposed initial limits: five seconds for
focus/preparation, two seconds for first audio after arming, 500 ms maximum
unacknowledged audio age, one-second idle heartbeats, and two seconds to declare
the connection dead. The Mac stops active capture at 55 seconds; firmware has
an independent 60-second hard stop. Keep the existing ten-second transcription
finalization timeout. Qualification may tighten these values; document changes.

Prevent races at every await/callback: stop while preparing, delayed armed or
listening acknowledgements, Mac sleep/restart, and a second device or legacy
USB start must never reopen or take ownership of a canceled session.

## PCM framing, buffering, and audio ownership

Each binary message contains one 20 ms block: 960 mono signed little-endian
16-bit samples (1,920 bytes). Define a fixed 36-byte header: four-byte magic,
one-byte version, one-byte flags, two-byte header length, 16-byte session UUID,
32-bit sequence, and 64-bit first-sample index. Header integers use network
byte order; PCM stays little-endian. Sequence and sample index start at zero.
Format negotiation is fixed to this profile in v1; unknown flags or formats fail.

Cap complete binary messages at 1,956 bytes and JSON control messages at 4 KiB.
Handle WebSocket fragmentation/reassembly within those limits; a transport
callback is not necessarily a complete application frame. Reject duplicate,
out-of-order, skipped, malformed, or stale-session frames. Do not invent silence
to hide missing frames. The final sequence explicitly describes an empty stream
if the user stops before its first complete frame.

Use a firmware ring of ten frames (200 ms; 19,200 PCM bytes) and a Mac ingress
queue of at most 25 frames (500 ms; 48,000 PCM bytes). Account separately for
headers, TLS/WebSocket buffers, I2S DMA, task stacks, and Speech-owned memory.
Limit in-flight sends and application acknowledgements too; an API accepting a
send is not proof that the Mac consumed it. Overflow or acknowledgement timeout
ends the session. Never hold a display/I2C lock during network I/O.

Refactor `voice_audio.c` so exactly one task reads the microphone codec. USB
callbacks consume that task's ring without blocking on capture; underruns emit
silence. Wireless sending consumes the selected stream from its worker task.
Separate physical capture authorization from USB host mute: host mute affects
USB output only. In Wi-Fi mode, any enumerated USB endpoint emits silence, and
USB mute changes cannot silence Wi-Fi. Clear rings and sample ownership at every
stop or transport change so pre-start/stale audio cannot leak into a later session.

Coordinate the speaker's shared I2S clock and codec lifecycle. Suppress attention
chimes while starting/recording/finishing dictation and serialize speaker open/close
against capture. Validate clock stability and capture continuity with pending chimes.

## Mac integration and task identity

Split the current recorder into USB and wireless audio sources feeding a common
recognition engine. Convert validated wireless frames into owned `AVAudioPCMBuffer`
instances and append them through Speech's audio-buffer API on one serial queue.
Keep transcript accumulation, input-level calculation, local-only recognition,
duration limits, and generation checks shared across both sources.

Route legacy IPC and WSS starts through one coordinator and busy check. Store
the explicit task/session identity independently of passive “current task”
observation. Multiple presented views must not erase a session's acknowledged
target. A freshly confirmed switch to another task, loss of the trusted Codex
connection, or unsupported host ends capture and preserves partial text for
review. Bound uncertain observations with liveness checks; never infer a target
from a title, recent-task order, or cached attention selection.

Add an optional `wirelessMicrophone` capability and a separate active-session
summary (session ID, transport, state, revision, error code) to private IPC and
bridge responses. Keep legacy `desktopVoiceHotkey` tied to USB start support;
otherwise old firmware could attempt USB capture when only Wi-Fi is ready.
Updated firmware uses WSS session acknowledgements for wireless recording and
retains the existing polling stop rules for USB. Stale bridge snapshots cannot
override a newer wireless session revision. Old firmware/old companions continue
USB behavior or report “Wireless update required”.

Keep existing icon colors: amber starting, green listening, red error, gray
idle/muted. Display the failure reason until dismissed or retried so the next
attention refresh does not hide it. Show “Pair Mac”, “Mac offline”, “Wi-Fi lost”,
or “Speech unavailable” as appropriate, rather than a USB-permission prompt.

## Implementation sequence and affected files

| Step | Deliverable | Completion gate |
| --- | --- | --- |
| 1. Transport feasibility | Small native WSS listener and ESP32 client; TLS provisioning and bounded synthetic PCM reception; verify supported library versions. | Physical battery-powered board connects with verified TLS; invalid identity/credential fails; receiver accepts correctly framed PCM. |
| 2. Shared recognition | Extract `DictationAudioSource`, USB adapter, and recognition engine from `DictationRecorder.swift`; adapt `DictationModel.swift`. | Existing USB dictation, accumulation, stop races, and composer handoff still pass. |
| 3. Wireless receiver | Add `WirelessMicrophoneServer.swift`, protocol parser, pairing store, and shared coordinator; update app lifecycle, `VoiceSettingsView.swift`, and `BridgeController.swift`. | Auth, lifecycle, bounded ingress, local Speech, and session ownership pass on macOS. |
| 4. Firmware capture/transport | Refactor `voice_audio.c` and `usb_microphone.c`; add `wireless_microphone.c/.h` and protocol/state helpers; update `voice_control.c`, `main.c`, CMake, Kconfig, and managed dependencies. | One codec reader, bounded queues, immediate local mute, no duplicate USB/Wi-Fi samples, and deterministic timeout tests pass. |
| 5. Integration and compatibility | Add IPC/session summary fields and bridge normalization; update `attention_client.c`, `attention_model.h`, and `attention_ui.c`; implement source choice and chime coordination. | USB-only older versions retain behavior; wireless state survives stale attention polls; error reasons remain visible. |
| 6. Qualification and docs | Update `docs/protocol.md`, architecture, root/macOS READMEs, provisioning instructions, and CI tests. | Battery-only end-to-end matrix below passes on recorded firmware/app revisions. |

Each step should be a reviewable change with dependencies in this order. Synthetic
fixtures in step 1 are transport evidence only. Do not mark the feature complete
until physical microphone speech reaches the intended composer over Wi-Fi.

## Validation and acceptance

Automated checks:

- C and Swift shared protocol vectors: byte order, bounds, fragmented messages,
  session/sequence mismatch, overflow, and malformed controls.
- Deterministic state-machine tests with fake clocks: canceled starts, delayed
  responses, double stop, lost stop acknowledgement, expiry, reconnect, stale
  callbacks, and active-session revisions versus attention snapshots.
- Mac source/coordinator tests: USB unavailable with Wi-Fi ready, simultaneous
  starts, no fallback to a Mac microphone, no automatic replay, preserving
  multiple utterances, and failed-session partial text remaining for review.
- Native integration tests: authenticated WSS frames reach the shared audio sink;
  bad certificates/credentials and expired sessions deliver zero PCM to Speech.
  Run real Speech integration separately where permissions/models are available.
- Run `swift test --package-path macos`, bridge `npm test` and `npm run check`,
  firmware host CMake/CTest checks, release app build/signature verification, and
  the repository's ESP-IDF 5.4.4 build. Inspect firmware flash size and internal
  heap, stack watermarks, and TLS allocations with the actual display active.

Physical acceptance on the Waveshare ESP32-S3-Touch-AMOLED-2.06:

1. Boot on battery with USB physically absent. Pair/connect, select a task,
   dictate multiple phrases with pauses, and finish with either button. Verify
   the entire text appears once in that exact composer and remains unsent.
2. Repeat for 20 recordings across list/detail views and multiple Codex views.
   No startup failures, lost phrases, wrong-task text, or unbounded memory growth.
3. Record for the maximum duration. Confirm the Mac's 55-second stop and, under
   simulated lost host control, the device's independent 60-second stop.
4. Interrupt Wi-Fi, quit/restart the companion, sleep the Mac, switch tasks,
   congest the network, and stop during startup. Verify local gating, bounded
   termination, review-only partial text, and no capture on reconnection.
5. Test USB-only, Wi-Fi-only, and Auto; hot-plug USB in both idle and recording
   states, including a USB host mute. Confirm the session never changes source.
6. Exercise scrolling, display redraws, attention refreshes, and queued chimes
   during capture. No audio stalls, deadlocks, watchdog resets, or display damage.
7. Verify no wireless PCM before arming or after stop using receiver counters and
   controlled signal fixtures; verify no live USB PCM during Wi-Fi recording.
8. Measure warm-start time (target p95 under two seconds after the long-press
   event), audio queue age (normally under 200 ms), and stop gating (target one
   capture frame plus button polling latency). Measure battery draw over at least
   ten minutes idle-connected and ten minutes repeated recording. Report observed
   power/range/latency; do not claim a battery lifetime without measurement.

Log bounded operational metadata: timestamp, transport, stage/reason, sequence
counts, PCM peak/RMS, queue high-water marks, acknowledgement age, reconnect
counts, heap/stack watermarks, and duration. No audio, transcript, task IDs,
pairing secrets, or authorization headers in routine logs. Disable network
library payload dumps. Use synthetic samples for diagnostic fixtures.

## Delivery and rollback

Ship the companion with USB compatibility first, then update/provision firmware.
Wireless readiness remains false until pairing, listener health, and recognition
checks pass. Record exact app/firmware commits, build artifacts, and physical
acceptance results. The plan itself makes no claim about the currently flashed
firmware revision.

Rollback is to select USB mode or disable the wireless listener, with capture
stopped first. Keep the prior signed app and firmware artifacts available. Do
not erase Wi-Fi credentials or unrelated device settings during provisioning
or rollback. Certificate/credential rotation requires explicit re-pairing.

## API references checked for the proposed transport

- [Apple Network.framework WebSocket](https://developer.apple.com/documentation/network/nwprotocolwebsocket),
  [NWListener](https://developer.apple.com/documentation/network/nwlistener), and
  [TLS](https://developer.apple.com/documentation/network/nwprotocoltls): native
  connection/listener APIs; platform support and listener interoperability are
  verified in step 1.
- [Espressif WebSocket client documentation](https://docs.espressif.com/projects/esp-protocols/esp_websocket_client/docs/latest/index.html):
  WSS, certificate configuration, binary sends, and receive events. Configure
  certificate verification explicitly and validate the application version;
  transport defaults/subprotocol negotiation alone are insufficient.
