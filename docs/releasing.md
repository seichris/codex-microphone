# Releases and credential safety

The repository version is the single value in [`VERSION`](../VERSION). It is
mirrored in the bridge package metadata and the Mac app's
`CFBundleShortVersionString`. CI checks those copies on every push and pull
request. A release tag must be exactly `vVERSION`.

The release workflow builds two generic artifacts:

- a notarized `Codex ESP32 Display` Mac app, with bridge source but no bridge
  config or bearer token; and
- an ESP32-S3 firmware archive built from `firmware/sdkconfig.defaults`, with
  no Wi-Fi credentials, bridge token, pairing credential, or private key.

The firmware archive is intentionally generic and is useful for hardware
smoke-testing. The current firmware still compiles Wi-Fi and pairing values
into the image, so a normal connected board must be built locally with those
values before flashing. The Mac app creates its per-user bridge config on first
launch. **Copy Bridge Token** in the menu exposes that token for provisioning
without putting it in a release file. A future runtime-provisioning/OTA
release can make the generic image directly installable without a local build.

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

The tag workflow expects these GitHub Actions secrets for the Mac artifact:

- `APPLE_CERTIFICATE_BASE64` and `APPLE_CERTIFICATE_PASSWORD` for a Developer
  ID Application certificate;
- `APPLE_SIGNING_IDENTITY` containing the certificate's exact identity; and
- `APPLE_API_KEY_BASE64`, `APPLE_API_KEY_ID`, and `APPLE_ISSUER_ID` for
  `notarytool`.

After configuring those secrets and enabling the repository's immutable release
setting, update `VERSION`, run the local checks, and push the matching tag:

```bash
python3 scripts/check_no_secrets.py
python3 scripts/check_version.py
git tag "v$(cat VERSION)"
git push origin "v$(cat VERSION)"
```

The workflow validates the tag, builds the firmware and app, notarizes the Mac
archive, generates SHA-256 checksums, and publishes the release assets. A tag
with a mismatched version or a failed credential check stops before publishing.
