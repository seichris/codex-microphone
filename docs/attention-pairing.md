# Durable attention bridge pairing (issue #11)

The attention/control connection now uses runtime pairing, dedicated HTTPS, and
Bonjour discovery. It is independent of the native WSS microphone protocol and
of the USB Audio Class interface. Attention filtering, thread IDs and selection
semantics are unchanged.

## One-time prerequisites and safety boundary

Use Node.js 22, Python 3.11+, OpenSSL, and ESP-IDF 5.4.4 or 5.5. The macOS bridge
uses the system `dns-sd` command; Python's kernel `flock` helper prevents two
bridge processes from writing one pairing store and releases the lock on crash.

**An owner/factory-provisioned HMAC_UP eFuse key is required for encrypted device
storage.** `CONFIG_CODEX_ATTENTION_PAIRING_HMAC_KEY_ID` defaults to block 5. Before
choosing it, inspect the actual board's eFuse assignments with Espressif tooling.
Provision a cryptographically random, read-protected HMAC_UP key into an unused
supported block using the ESP-IDF factory/security process, and select the same
block in firmware. eFuse programming is irreversible: do not reuse a block
assigned to flash encryption, secure boot, or another application. This PR does
not run or supply a blind eFuse-burning script. See Espressif's
[NVS encryption documentation](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-reference/storage/nvs_encryption.html).

The firmware **only reads** the configured key; it never generates an eFuse key,
erases an invalid record, or downgrades to plaintext storage. An unprepared board
shows `Pairing storage unavailable`; the existing USB microphone remains usable.
This is a deployment prerequisite, not an automatic first-boot operation. The
new `attn_pair` NVS partition is appended after existing storage so previous
wireless pairing namespaces and partition offsets are not moved. A one-time
firmware/partition-table update is required; normal reconnects need no rebuild.

The default legacy `nvs` partition is initialized explicitly without changing
its existing format. Do not replace this with `nvs_flash_init()`: with HMAC NVS
encryption enabled that API can automatically program an empty eFuse. The
global default key selector is deliberately left at its unsupported sentinel 6;
only the dedicated partition uses the explicitly configured supported key.

Encrypted NVS plus a hardware HMAC detects record corruption and protects an
offline flash dump. It is not protection against malicious replacement firmware,
restoration of an old valid flash image, or compromise of the paired Mac. Use an
appropriate secure-boot/flash-encryption factory policy for production devices.
On the Mac, bridge private keys and per-device secrets live in a mode-0700
`.attention-pairing` directory with a mode-0600 store; they are not Keychain-backed.
Protect backups and do not roll back revocation state from an old backup.

## Pair once

The authenticated local transport is a physically connected **UART0 maintenance
channel**, separate from the native USB microphone. Use a 3.3 V USB-UART adapter
with a common ground, adapter RX to ESP32 UART0 TX, and adapter TX to UART0 RX.
The ESP32-S3 default UART0 pins are GPIO43/TX and GPIO44/RX; verify access and
routing against the exact Waveshare board revision before connecting. Never
apply 5 V to these GPIOs. The existing USB-C UAC interface is not a serial port
and its descriptors have not been changed by this implementation.

From the repository root:

```sh
python3 -m pip install -r scripts/requirements-attention.txt
cd bridge
npm run setup                 # once for a new Mac configuration
npm start                     # or start the bridge with the Mac companion
```

Do not run two bridge instances against the same store. In another terminal:

```sh
cd bridge
npm run pair -- --port /dev/cu.usbserial-EXAMPLE
```

The CLI reads the local administrative credential from the owner-only config,
prompts for Wi-Fi credentials (password input is hidden), obtains the Mac's
identity/trust anchor over loopback, and sends them directly through UART0.
It prints the **public** bridge ID and certificate SHA-256, never a Wi-Fi
password, device secret, admin token, or credential-bearing URL.

Check the Mac identity shown on the device. Release BOOT, then make a **fresh
1.5-second BOOT hold**. A button held before the prompt is not confirmation;
PWR cancels and the prompt expires after 60 seconds. Recording is revoked while
the prompt is active. Only confirmation permits the sealed record to be written.
The device acknowledges the committed record and restarts. An interrupted or
lost acknowledgement can be retried without issuing a second device identity.

The device discovers `_codex-attention._tcp`, accepts only TXT records with its
paired `id`, `v=1`, and `tls=1`, and validates HTTPS using its paired certificate
and `attention-<bridgeId>.local` identity, independently of the discovered IP.
An advertiser claiming the right ID but using another certificate is rejected.
No HTTP, shared-token, default-CA, or unverified-host fallback exists.

Version 1 discovery uses IPv4 candidates, bounded queries, a 30-second address
cache and 5-second HTTP timeouts. A failed transport invalidates the cache, so
normal polling rediscovers after DHCP changes or service reappearance. On
networks that block multicast, set `fallbackHost` in `bridge/config.json` **before
pairing** to a stable DHCP/DNS hostname (optionally backed by a router reservation).
An empty value disables this fallback. The same pinned identity is still
required; the hostname is a locator, not a trust decision. A `.local` hostname
itself also depends on mDNS, so use ordinary DNS when multicast is unavailable.

## Reset, re-pair, and revocation

```sh
npm run pair -- --port /dev/cu.usbserial-EXAMPLE --reset
npm run pair -- --port /dev/cu.usbserial-EXAMPLE --replace
```

Reset requires another fresh device confirmation. It first atomically records
`RESETTING` with a random nonce and blocks normal attention requests. The bridge
revokes the old identity durably, then returns a nonce-bound authenticated
acknowledgement. Only this acknowledgement allows the device to persist an empty
record, clearing its identity, Wi-Fi settings, trust anchor and secret. A later
pair creates a fresh random device identity. Same-Mac replacement also advances
the credential generation and revokes competing pending replacements.

**Offline reset is pending, not complete.** Reconnect the original Mac/LAN, or
use the CLI on the original Mac to relay its authenticated revocation ACK over
UART0. Power loss retains the pending nonce. Repeating the ACK is safe. A new or
impersonating Mac cannot forge completion. If the original Mac's pairing store
is permanently lost, this flow cannot establish revocation on that lost Mac;
owner-controlled recovery is required rather than falsely reporting success.

Network tasks only enqueue an ACK in a bounded mailbox. The internal-RAM-stack
provisioning actor verifies it and writes NVS using an internal-RAM staging
buffer; the PSRAM-backed network task never writes flash. A flash-write failure
blocks storage use until reboot/revalidation, since its durable outcome can be
uncertain. There is one authenticated, versioned NVS blob, not independently
updated fields or automatic fallback to an older generation.

## Migration from sdkconfig provisioning

Stop the old bridge and Mac companion. Keep existing wireless-microphone bundles
and sdkconfig WSS fields unchanged. Then run:

```sh
cd bridge
npm run setup -- --rotate-admin
```

This atomically generates a separate admin token, forces the administrative
listener to loopback, preserves unrelated configuration, and prints no secret.
Remove an old `CODEX_ATTENTION_TOKEN` environment override as well, or it will
continue to override the newly generated config token. Restart the bridge and
companion, update firmware/partition table once, satisfy the owner key
prerequisite, and pair through UART0 as above.

`CONFIG_CODEX_ATTENTION_BRIDGE_URL` and `CONFIG_CODEX_ATTENTION_BRIDGE_TOKEN` are
retained only as explicitly ignored migration settings. They are never used by
the attention client. Delete those values from private sdkconfig/build backups;
old flashed binaries and copies can still contain the former shared token.
Do not import that bearer into the new pairing record. Re-enter Wi-Fi once via
the private CLI prompt; subsequent credentials come from encrypted NVS. Legacy
Wi-Fi settings are used only for an independently configured wireless microphone
when no attention record exists. A paired attention record supplies the shared
radio's Wi-Fi credentials without changing the WSS microphone's TLS or key.

## Authentication protocol

Administration/dashboard: HTTP `127.0.0.1:5180`, separate owner token, no device
access. Paired device API: HTTPS port 5182 by default. The WSS microphone keeps
its independent default port 5181. Device credentials cannot call `/refresh`,
admin endpoints or the dashboard, and admin credentials cannot authenticate as
a device. Requests with query strings/credential URLs are rejected.

A device POSTs `{deviceId,generation}` to `/api/v1/device/challenge`. The bridge
returns `{version:1,bridgeId,sessionId,challenge,expiresIn:30}`. Device IDs are
32 lowercase hex characters; secret, session and challenge are 64 hex characters.
The token request to `/api/v1/device/token` includes those request fields,
`sessionId`, `challenge`, and a lowercase SHA-256 HMAC `proof` over:

```text
codex-attention-auth-v1\n<bridgeId>\n<deviceId>\n<generation>\n<sessionId>\n<challenge>
```

There is no final newline; the HMAC key is the **decoded hexadecimal secret**.
Challenges are single-use and expire after 30 seconds. Access tokens last 300
seconds, are held in RAM, and carry the bridge process's random session identity.
Each accepted device request includes the bearer token plus `X-Codex-Device`,
`X-Codex-Generation`, `X-Codex-Session`, and a strictly increasing canonical
`X-Codex-Sequence` decimal integer. Duplicated/stale sequences, stale generations,
revoked devices, old sessions and superseded tokens are rejected before action
execution. The firmware renews 15 seconds before expiry. A bridge restart is
recovered through one bounded authentication retry after a pre-execution 401.
**A command with an ambiguous network/lost-response failure is never replayed.**

For `/api/v1/device/revoke`, the signed message uses domain `revoke` and only the
reset nonce after the generation. The response proof uses domain `revoked` with
the same fields. The inert revoked device key is retained on the Mac only to
reproduce a lost acknowledgement, never to issue another access token.
Per-peer authentication rate limits and bounded challenge/device stores prevent
unbounded authentication allocations. Ordinary health, list and diagnostics
responses never include pairing keys, trust bundles or admin credentials.

## Diagnostics and qualification

Distinct device states cover unpaired, secure-storage unavailable, Wi-Fi
unavailable, service undiscovered, TLS trust failure, authentication failure,
bridge unavailable, and reset pending. A valid response with no current Mac
selection stays the existing unavailable/current-thread UI state; it is not an
authentication error. Security-boundary failures clear cached cards/voice target;
ordinary transient network failures preserve the last list with an error.

Automated commands (also run in CI):

```sh
(cd bridge && npm test && npm run check)
python3 scripts/test-attention-provisioning.py
python3 firmware/tests/run_attention_pairing_tests.py
python3 firmware/tests/run_wifi_manager_tests.py
python3 firmware/tests/run_voice_audio_tests.py
python3 firmware/tests/run_wireless_lifecycle_tests.py
```

The C tests compile the **production** record, NVS coordination, confirmation,
discovery, connection and attention-parser code. Host adapters model NVS/eFuse,
Wi-Fi and mDNS; OpenSSL implements host HMAC and libcurl performs real HTTPS
against the actual Node bridge. Tests cover migration and malformed records,
physical-confirmation freshness/timeouts, flash errors, restart and address
change, fallback, wrong trust, unknown service/version, expiry, replay,
revocation, reset acknowledgements and no replay after an executed command's lost
reply. These are not measurements of ESP-IDF's TLS/NVS stack or board hardware.

Physical release gate — **not completed by host tests**:

- [ ] Verify exact board UART routing, secure-key provisioning, partition update,
      memory headroom and confirmation/cancellation with USB audio active.
- [ ] Pair once; cold boot repeatedly and reconnect without rebuilding firmware.
- [ ] Change real DHCP address/router configuration; restart/kill the Mac bridge;
      remove/reintroduce its mDNS service and disconnect/reconnect Wi-Fi.
- [ ] Test multicast-blocked fallback, wrong advertised identity/certificate,
      token renewal/revocation, and absence of a current Mac selection.
- [ ] Reset/re-pair with power loss before/after NVS writes and bridge ACK; verify
      pending offline reset, stale-credential rejection, and new identity.
- [ ] Exercise the existing paired WSS microphone and native USB fallback,
      including stop/start races and simultaneous secure attention polling.
