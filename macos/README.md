# Codex ESP32 Display menu-bar app

This native SwiftUI companion owns the local Node bridge and exposes it as a
macOS menu-bar app. Its status-item icon is a monochrome filled silhouette of
the 2.06 device.

Build the app from the repository root:

```bash
./macos/build_app.sh
open "macos/build/Codex ESP32 Display.app"
```

The app starts `bridge/src/index.mjs` and writes its output to
`~/Library/Logs/CodexESP32Display/bridge.log`. The menu provides bridge status,
start/stop, the local dashboard, endpoint copying, log reveal, Desktop Voice
status, Voice Settings, and quit.

## Device dictation

Open **Voice Settings… → Enable Dictation Permissions**. Speech Recognition and
on-device English recognition are required for both transports. USB mode also
requires Microphone permission and the exact Waveshare USB device; Wi-Fi mode
does not depend on `AVCaptureDevice` enumeration or Mac Microphone permission.
Accessibility is not used for recording or draft handoff. The companion never
falls back to the Mac's built-in input or changes the system default microphone.

The **Microphone transport** picker supports `Auto`, `USB`, and `Wi-Fi`. Auto
chooses USB when it is ready and otherwise uses the paired Wi-Fi listener; the
choice is locked for the duration of a recording. USB insertion/removal cannot
reroute an active session.

### Pairing the Wi-Fi microphone

The native listener uses a local TLS identity and a distinct 256-bit credential
per board. Generate a private pairing bundle on the Mac (the output contains a
private key and must not be committed or shared):

```bash
scripts/create-wireless-pairing-bundle.sh \
  --host <mac-lan-ip-or-dns-name> \
  --output "$HOME/wireless-pairing-CESP32VOICE01.json"
```

Import that JSON from **Voice Settings… → Import Wi-Fi pairing bundle…**. The
identity and credential are stored in Keychain; only non-secret metadata is
kept in preferences. The listener defaults to port 5181 and requires the
certificate SAN to match the configured host.

Provision the same bundle into the firmware build configuration before
flashing:

```bash
scripts/provision-wireless-microphone.sh \
  "$HOME/wireless-pairing-CESP32VOICE01.json" firmware
cd firmware
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.wireless.defaults" idf.py reconfigure build
```

The generated `firmware/sdkconfig.wireless.defaults` is mode 600 and ignored
by Git. It is not included in the application bundle or routine logs. A fresh
pairing is required after changing the Mac identity, host, or credential.

1. Select a task on the device and hold either button for one second.
2. Wait for `LISTENING`. USB requires a real sample buffer from the exact
   device; Wi-Fi requires the authenticated WSS session to be armed and the
   first correctly framed PCM block to arrive. A posted control acknowledgement
   alone never opens the recorder.
3. Speak, then hold again. This closes the device PCM gate and calls
   `endAudio()` on the local speech request so transcription can finish.
4. Completed dictation automatically opens in the recorded task's composer,
   using its task ID and a percent-encoded `prompt` query in a Codex deep link.
   This replaces any existing composer text; it never sends a message.
   After Codex accepts the handoff, the companion clears its local copy so the
   next recording can start immediately. Failed recognition or handoff stays in
   Device Dictation for review rather than opening automatically.

While the device is listening or finishing recognition, the companion shows a
small non-activating overlay centered near the bottom of the active display.
It follows the FluidVoice pattern: a dark rounded panel, live input waveform,
status, and a short tail of the current transcript. The larger Device Dictation
window appears again when review or an error needs attention.

The exact target is pinned for a recording. Switching targets while recording,
switching tasks during a recording, and late callbacks from previous recordings
are rejected. A new device recording replaces a prior failed handoff. Recording
ends after 55 seconds; finalization has a 10-second timeout
and retains partial text on failure. Empty speech results, including an empty final
result after stopping, preserve the latest non-empty transcript for review. Late
callbacks cannot overwrite the editable draft. The existing firmware uses `VOICE MUTED`
while the companion finishes transcription and `VOICE READY` when the companion is ready.
On-device Speech can reset its current transcription after a pause without ending
the request; the companion accumulates those completed partial utterances so one
long-press session keeps all spoken phrases.
The updated firmware closes its gate when a fresh companion snapshot reports
that recording ended or became unavailable, and enforces a 60-second local
limit even if networking fails. Older firmware still needs the second physical
press to close the gate after automatic host-side completion.

The running app with bundle ID `com.openai.codex` is preferred when opening task
links, because a newer ChatGPT.app and an older Codex.app can coexist. If multiple
copies are running and none is active, handoff fails rather than guessing.
The companion clears its local text once macOS accepts the URL; the public deep-link
API has no separate acknowledgement that the composer rendered it.
Manual task selection now follows Codex's local task-presentation events. The
companion verifies the local broker, tracks exact task IDs per source client,
reconnects on failures, and clears ambiguous or unavailable selection. Multiple
presented task views are treated as ambiguous; remote task hosts are not yet
supported. This private interface is version checked and requires no Accessibility
permission. Voice Settings → **Show Detection Log** reveals the bounded
operational log. See [`docs/focused-task-sync.md`](../docs/focused-task-sync.md).

Diagnostics are written to
`~/Library/Logs/CodexESP32Display/dictation.log`: timestamps, stages, sample
counts and input peaks only. No audio, transcript text, task IDs or credentials
are logged. Inspect that file for capture/finalization evidence; old permission
errors in `bridge.log` are not evidence of a new dictation failure.

Local validation: `swift test --package-path macos` covers session ownership,
stale callbacks, partial-text preservation, no-speech failures, stop/start races,
and exact target/text URL encoding. The release app build and live USB speech
acceptance remain separate checks.

The private bridge/controller channel is a per-launch, mode-0700 temporary
directory with a random token. Desktop-control commands are not exposed as an
unauthenticated local socket.

The build embeds the bridge source and current `bridge/config.json` in the app
bundle so a Finder launch does not depend on access to the protected workspace.
Opening the dashboard from the menu automatically seeds its token through a
URL fragment, then removes the fragment from the address bar after saving it in
the dashboard's local storage. Rebuild after changing the bridge config.

The previous `com.seichris.codex-esp32-display` LaunchAgent should remain
unloaded while this app owns port `5180`.

The Wi-Fi microphone listener is owned by this app on port `5181`. It is
optional: an unpaired or unavailable listener leaves USB behavior intact.

## Stable local signing

For repeated local builds, choose an existing Apple Development signing identity
using `CODEX_DISPLAY_SIGN_IDENTITY` or place its fingerprint in the ignored
`macos/.signing-identity` file. The build fails if the chosen identity cannot sign;
it does not silently fall back to ad hoc signing. Without a selection, the build
uses an ad hoc signature and warns that privacy permissions may need reapproval
when the binary changes. No certificate or private key is stored in this repo.

Changing from ad hoc to certificate signing requires one fresh permission grant.
Keep the app's installation path and signing identity stable for later updates.
The build verifies its completed signature. Do not weaken the designated
requirement to an identifier-only rule to work around privacy checks.
