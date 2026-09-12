# Codex Microphone

![Codex Microphone](docs/codex-mic-github.jpg)

A compact Codex Desktop companion for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
It shows unread and pinned attention threads and turns the board into a
privacy-gated USB or paired Wi-Fi microphone for the selected task.

The GitHub repository is `seichris/codex-microphone`; the local folder and
Codex project intentionally remain `codex-esp32-display` so existing threads
continue to use the same local project.

## Controls

- **Upper button — short press:** move to the next thread.
- **Lower button — short press:** open the selected thread; press again to return.
- **Either button — hold for one second:** focus the selected thread and start
  dictation; hold again to stop. The Mac companion opens the draft in that task
  (FluidVoice results first open for review).
- **Touch:** scroll, open cards, select Voice Target, and adjust display settings.
- **Lower button — longer hardware hold:** power off the board.

![Codex Microphone controls](docs/codex-microphone-controls.jpg)

## Get started

- **Prerequisites:** macOS with Codex Desktop or CLI, Node.js 22, Python 3.11+, OpenSSL, and
  ESP-IDF 5.4.4 or 5.5.
- **Start the bridge:** clone into the existing local name, then run:

  ```bash
  git clone https://github.com/seichris/codex-microphone.git codex-esp32-display
  cd codex-esp32-display/bridge
  npm run setup
  npm start
  ```

- **Prepare and pair the board once:** follow [secure attention pairing](docs/attention-pairing.md)
  for the initial firmware/partition update, owner-provisioned HMAC storage key,
  and physically confirmed USB CDC pairing. Wi-Fi, bridge trust and device identity
  are stored in encrypted NVS; do not embed an attention URL or token in firmware.
  Routine reconnects, DHCP changes, bridge restarts and token renewal need no rebuild.
  The native USB-C cable carries both the microphone and the pairing serial interface.
- **Enable paired Wi-Fi dictation (optional):** create and provision a pairing
  bundle with the scripts described in [macOS pairing instructions](macos/README.md#pairing-the-wi-fi-microphone).
- **Run the Mac companion (optional):** use `./macos/build_app.sh`, open the
  generated app, and grant Speech Recognition permission for Apple Speech (plus
  Microphone for USB mode). Alternatively, select the [FluidVoice engine](docs/fluidvoice.md)
  to transcribe using its local API.

Detailed implementation, architecture, API, security, and validation notes are in
[Current software](docs/current-software.md). The [device protocol](docs/protocol.md)
and [architecture notes](docs/architecture.md) remain available as references.
For versioned firmware and Mac companion artifacts, see [release and CI
operations](docs/releasing.md).
