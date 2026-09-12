# FluidVoice implementation plan, reconciled with main

Issue: [#5](https://github.com/seichris/codex-microphone/issues/5).
Original plan baseline: `deb13cfe817f77146dafcf0301fa6275ef8f3338` (2026-09-07).
Implementation baseline: GitHub main `f97d043ffe03d1e874b61b3ed038d01c04eea274`
(fetched 2026-09-12).

## Changes since the original plan

- PR #7 implemented paired TLS/WebSocket microphone transport into the native
  companion, bounded framing, session IDs and recording admission.
- PR #8 fixed certificate provisioning and wireless recovery.
- PR #9 fixed revised native Speech hypotheses so they replace guesses instead
  of duplicating them.
- PR #13 established release packaging and secret-safety checks.
- PR #14 added durable attention pairing/discovery/HTTPS, capture ownership and
  wireless lifecycle qualification. Existing code suppresses chimes, separates
  USB host mute from Wi-Fi capture and rejects stale session operations.

The proposed additional raw HTTP upload/session API in Node would duplicate a
working integration boundary and its security/session state. The implementation
therefore keeps paired WSS and integrates FluidVoice at the companion's existing
recorder sink. No firmware or bridge protocol change is required.

## Implementation decisions

| Original work item | Reconciled implementation |
| --- | --- |
| Capture without USB polling | Reuse main's dedicated capture producer and Wi-Fi source ownership. |
| Bounded LAN audio transfer | Reuse authenticated WSS frames, queue admission, sequence validation and drain barrier. |
| PCM to completed WAV | Add bounded `FluidVoiceAudio` RAM accumulation in the native recorder; retain the 48 kHz shared format. |
| Local transcription adapter | Add `FluidVoiceClient` in Swift instead of a Node proxy; numeric loopback, no redirects, bounded response, stable errors. |
| Mutual exclusion and original task | Reuse `DictationSession`, its generation ID and the shared model admission guard for both transports/engines. |
| Enablement and readiness | Add independent engine/transport settings, API port and health check; keep Apple Speech default. No automatic FluidVoice mutation. |
| Review before task handoff | FluidVoice uses the existing Mac review surface with explicit Open as Task Draft/Discard. Native auto-handoff and recovery remain intact. |
| Device transcript screen | Use the Mac review/editing workflow established by newer PRs. Device continues to show recording state; a second review UI on the device is not added. |
| Transcription job polling/retention | Native model owns one result and its task; no HTTP job polling or eight-record history is needed. Draft lasts until explicit action; no audio persistence. |
| Cancellation/retry | Invalidate UI generation, cancel recorder/upstream work, reject late results; an uncertain upstream call blocks further calls until explicit reset. |
| Cloud enhancement | Do not call postprocess; remains separate. |

USB input also supports FluidVoice using the same bounded recorder backend,
without changing the default microphone or requiring Apple Speech authorization.
The engine and port are captured for each take. The existing 55-second companion
cutoff is retained, with a hard 60-second PCM byte cap rather than silently
truncating to FluidVoice's limit.

## Delivery checklist

- [x] Compare GitHub main and merged PRs before implementing.
- [x] Add local HTTP client, format/size validation and uncertain-request admission.
- [x] Integrate the recorder after Wi-Fi drain and USB capture.
- [x] Add engine settings, readiness diagnostics and review-first FluidVoice handoff.
- [x] Add fixture and production-network tests without using live microphone audio.
- [x] Document setup, API contract, data handling and current architectural differences.
- [ ] Qualify real board audio and the installed FluidVoice model; see the
  [physical verification matrix](fluidvoice.md#verification-boundaries).

The PR supplies implementation and automated verification. Physical qualification
remains explicit unfinished evidence for issue #5, so the PR should reference the
issue without automatically closing it. It does not install an app, enable
FluidVoice, flash a device, or send a Codex message.
