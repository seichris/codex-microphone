# Releases and credential safety

The repository version is the single value in [`VERSION`](../VERSION). It is
mirrored in the bridge package metadata and the Mac app's
`CFBundleShortVersionString`. CI checks those copies on every push and pull
request. A release tag must be exactly `vVERSION`.

The release workflow builds two generic artifacts:

- an ad-hoc-signed `Codex ESP32 Display` Mac app, with bridge source but no
  bridge config or bearer token; and
- an ESP32-S3 firmware archive built from `firmware/sdkconfig.defaults`, with
  no Wi-Fi credentials, bridge token, pairing credential, or private key.

The firmware archive is intentionally generic. Attention Wi-Fi and bridge
credentials are now provisioned at runtime over a physically confirmed USB CDC
connection; they are not compiled into the image. Secure pairing storage still
requires the separately owner/factory-provisioned HMAC key described in
[attention pairing](attention-pairing.md), and physical qualification is a release
gate. Independent wireless-microphone WSS provisioning remains unchanged.

The Mac app creates its private per-user config on first launch. **Copy Local
Admin Token** exposes that local dashboard credential, not a device credential.
Use the companion's actual config path with the pairing CLI; see the pairing
guide. Never copy a device secret or admin token into a release file.

## CI checks

`python scripts/check_no_secrets.py` scans the Git tracked-file list. It rejects
secret-bearing config paths and credential-shaped Wi-Fi, bridge-token, bearer,
and inline assignments. It reports only paths, line numbers, and categories;
matched values are never printed. Deterministic values used by the checked-in
host and firmware fixtures are exact allowlisted test data, so a new real value
still fails even in a test directory.

The check is a repository guard, not a replacement for GitHub's [secret
scanning](https://docs.github.com/en/code-security/concepts/secret-security/secret-scanning)
and [push protection](https://docs.github.com/en/code-security/concepts/secret-security/push-protection).
Enable those repository settings as a second, server-side layer. If a
credential is ever committed, revoke or rotate it first and then remove it
from history; deleting the current line alone does not invalidate a leaked
token.

## Publishing a release

The Mac artifact is ad-hoc-signed for integrity but is not signed with a
Developer ID certificate and is not notarized. macOS may therefore show a
Gatekeeper warning on first launch; users can approve it in System Settings or
use Finder's Open action. Developer ID signing and notarization can be added
later without changing the release layout.

After enabling the repository's immutable release setting, update `VERSION`,
run the local checks, and push the matching tag:

```bash
python3 scripts/check_no_secrets.py
python3 scripts/check_version.py
git tag "v$(cat VERSION)"
git push origin "v$(cat VERSION)"
```

The workflow validates the tag, builds the firmware and app, generates SHA-256
checksums, and publishes the release assets. A tag with a mismatched version or
a failed safety check stops before publishing.
