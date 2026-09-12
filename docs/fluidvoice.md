# FluidVoice transcription

The Mac companion can use FluidVoice's local transcription API with either the
paired Wi-Fi microphone or the Waveshare USB microphone. Apple Speech remains
the default. Engine selection and transport selection are independent and fixed
for each recording; failures never switch engines or inputs automatically.

## Setup

1. Install/open FluidVoice and select/download its speech model. This integration
   was checked against the v1.6.9 source and API contract; health alone does not
   prove the model is ready.
2. Enable FluidVoice's hidden local API preference, then quit and reopen
   FluidVoice yourself. For the v1.6.9 bundle (`com.FluidApp.app`):

   ```sh
   defaults write com.FluidApp.app LocalAPIEnabled -bool true
   ```

   To disable it later, set the same preference to `false` and relaunch FluidVoice.
   The companion never modifies these preferences, installs or restarts FluidVoice.
3. Open **Voice Settings → Transcription engine → FluidVoice (local API)**.
   The default local port is **47733**; change it only if you also configured
   FluidVoice's `LocalAPIPort`. Only `127.0.0.1` is used.
4. Choose **Check FluidVoice**. A reachable API/version is distinct from successful
   transcription. The first recording checks the selected speech model.
5. Select **Wi-Fi** for battery-only operation and use the existing
   [microphone pairing](../macos/README.md#pairing-the-wi-fi-microphone). Wi-Fi
   FluidVoice mode needs neither Apple Speech nor Mac Microphone permission.
   USB still requires Mac Microphone permission and the exact Waveshare input.

## Record and review

Select a task and hold either device button for one second. Wait for listening,
then speak. Hold again to finish. The existing transport drains admitted frames
before transcription; no text is streamed from FluidVoice during recording.
The companion stops recording after 55 seconds, with a separate 60-second audio
byte ceiling and the board's existing cutoff.

The result appears in the companion's **Device Dictation** window, labeled with
the task selected when recording began. Edit it and choose **Open as Task Draft**
or **Discard** before recording again. Opening a draft replaces composer text;
it never sends the message. A link-open result is not a composer-delivery
acknowledgement. The existing memory-only handoff recovery copy is retained.
Native Apple Speech retains its existing automatic draft-opening behavior.

Use **Open Dictation → Cancel recording** to cancel capture or an in-progress
transcription. The UI immediately rejects late results for the cancelled take.
FluidVoice has no inference-cancellation endpoint: aborting the HTTP request
cannot establish that inference stopped. After an uncertain timeout/interruption,
check FluidVoice and choose **Allow another FluidVoice request** in Voice
Settings. Health checks do not clear that latch and no audio request is retried
automatically. This latch lasts for the current companion process.

## Data path and limits

```text
Waveshare → paired TLS WebSocket (or USB) → native companion
    → bounded PCM in RAM → complete WAV → 127.0.0.1:47733/v1/transcribe
    → original-task review → explicit task draft action
```

- Reuses the WSS preparation/commit/armed handshake, sequence validation, bounded
  ingress, Stop drain barrier, capture ownership and reconnect behavior already
  present on main. No new firmware build or second Node audio-upload API is needed.
- Keeps 48 kHz mono PCM16: at most 5,760,000 PCM bytes, plus a 44-byte WAV header.
  This stays below FluidVoice v1.6.9's 25 MiB request and five-minute audio limits.
  Model inference performs its own resampling. RAM copies remain bounded by the
  single-take limit; no companion audio files are created.
- Calls only `/v1/health` and `/v1/transcribe`. Uses an ephemeral HTTP session,
  disables proxies/cookies/cache, rejects redirects, and caps responses while
  reading (4 KiB health, 64 KiB transcription, 16 KiB UTF-8 transcript).
- Health requests have a two-second deadline; transcription has a 120-second
  resource deadline. One upstream request may run at a time. Busy, invalid,
  oversized, empty and failed responses become recoverable errors.
- Audio and transcript bodies are not logged. FluidVoice itself writes uploaded
  audio to temporary files and attempts deletion after decoding; deletion after
  an app crash is not guaranteed. This is not a claim of zero disk writes by
  FluidVoice or zero copies in system memory.
- `/v1/postprocess` is not called. Cloud enhancement and arbitrary remote
  transcription servers are outside this integration.

## Verification boundaries

Automated coverage includes the real Swift recorder's Wi-Fi ingress/drain with
synthetic PCM, a production TLS/WebSocket server connection into that recorder,
real loopback HTTP WAV upload and response handling, redirect rejection,
chunked response size enforcement, byte limits, cancellation, duplicate results,
original-task review and native handoff regressions.

These tests do not run the installed FluidVoice model or the physical Waveshare.
Before device qualification, record exact firmware/companion SHAs, board revision,
macOS/FluidVoice versions and model. Measure 5/30/50-second speech on battery,
warm/cold Stop-to-result latency, 20 repeated sessions, Wi-Fi loss, USB hotplug,
chime suppression and cancellation. Confirm correct task review and no automatic
send. Report hardware, inference, local tests and GitHub CI separately.

API source: [inference controller](https://github.com/altic-dev/FluidVoice/blob/v1.6.9/Sources/Fluid/Services/LocalAPI/InferenceAPIController.swift),
[configuration](https://github.com/altic-dev/FluidVoice/blob/v1.6.9/Sources/Fluid/Services/LocalAPI/LocalAPIModels.swift),
[audio decoder](https://github.com/altic-dev/FluidVoice/blob/v1.6.9/Sources/Fluid/Services/LocalAPI/LocalAPIAudioDecoder.swift).
