#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  printf 'Usage: %s <pairing.json> [firmware-directory]\n' "$0" >&2
  exit 2
fi
BUNDLE="$1"
FIRMWARE_DIR="${2:-$(cd "$(dirname "$0")/../firmware" && pwd)}"
[[ -f "$BUNDLE" ]] || { printf 'Pairing bundle does not exist: %s\n' "$BUNDLE" >&2; exit 1; }
[[ -d "$FIRMWARE_DIR" ]] || { printf 'Firmware directory does not exist: %s\n' "$FIRMWARE_DIR" >&2; exit 1; }
command -v python3 >/dev/null || { printf 'python3 is required.\n' >&2; exit 1; }
command -v jq >/dev/null || { printf 'jq is required.\n' >&2; exit 1; }

BOARD_ID="$(jq -er '.boardID | strings' "$BUNDLE")"
HOST="$(jq -er '.host | strings' "$BUNDLE")"
PORT="$(jq -er '.port | numbers' "$BUNDLE")"
SERVER_NAME="$(jq -er '.serverName | strings' "$BUNDLE")"
CERTIFICATE="$(jq -er '.serverCertificatePEM | strings' "$BUNDLE")"
CREDENTIAL="$(jq -er '.credential | strings' "$BUNDLE")"
[[ "$BOARD_ID" =~ ^[A-Za-z0-9._:-]{1,96}$ ]] || { printf 'Invalid board ID in pairing bundle.\n' >&2; exit 1; }
[[ -n "$HOST" && "$HOST" != *[/[:space:]]* && "$PORT" =~ ^[0-9]+$ && "$PORT" -ge 1 && "$PORT" -le 65535 ]] || {
  printf 'Invalid host or port in pairing bundle.\n' >&2
  exit 1
}
[[ -n "$SERVER_NAME" && "$SERVER_NAME" != *[/[:space:]]* && "${#SERVER_NAME}" -le 255 ]] || {
  printf 'Invalid TLS server name in pairing bundle.\n' >&2
  exit 1
}
[[ "$CREDENTIAL" =~ ^[0-9A-Fa-f]{64}$ ]] || { printf 'Pairing credential must be 256-bit hexadecimal.\n' >&2; exit 1; }
[[ "$CERTIFICATE" == *'-----BEGIN CERTIFICATE-----'* && "$CERTIFICATE" == *'-----END CERTIFICATE-----'* ]] || {
  printf 'Pairing bundle does not contain a PEM certificate.\n' >&2
  exit 1
}

for endpoint_name in "$HOST" "$SERVER_NAME"; do
  [[ "$endpoint_name" =~ ^[A-Za-z0-9]([A-Za-z0-9.-]{0,253}[A-Za-z0-9])?$ ]] || {
    printf 'Host and TLS server name must be DNS names or IPv4 addresses.\n' >&2
    exit 2
  }
done

json_string() {
  jq -er --arg key "$1" '.[$key] | strings | @json' "$BUNDLE"
}

URL="wss://$HOST:$PORT"
TARGET="$FIRMWARE_DIR/sdkconfig.wireless.defaults"
TEMP="$(mktemp "$FIRMWARE_DIR/.sdkconfig.wireless.defaults.XXXXXX")"
cleanup() { rm -f "$TEMP"; }
trap cleanup EXIT
umask 077
{
  printf '# Generated from a wireless pairing bundle; do not commit.\n'
  printf 'CONFIG_CODEX_ATTENTION_WIRELESS_ENABLED=y\n'
  printf 'CONFIG_MBEDTLS_HAVE_TIME_DATE=y\n'
  printf 'CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y\n'
  printf 'CONFIG_CODEX_ATTENTION_VOICE_TRANSPORT_AUTO=y\n'
  printf 'CONFIG_CODEX_ATTENTION_WIRELESS_DEVICE_ID=%s\n' "$(json_string boardID)"
  printf 'CONFIG_CODEX_ATTENTION_WIRELESS_URL=%s\n' "$(printf '%s' "$URL" | jq -Rr @json)"
  printf 'CONFIG_CODEX_ATTENTION_WIRELESS_SERVER_NAME=%s\n' "$(json_string serverName)"
  printf 'CONFIG_CODEX_ATTENTION_WIRELESS_CA_PEM=%s\n' "$(json_string serverCertificatePEM | sed 's/\\n/\\\\n/g; s/\\r/\\\\r/g')"
  printf 'CONFIG_CODEX_ATTENTION_WIRELESS_CREDENTIAL=%s\n' "$(json_string credential)"
} > "$TEMP"
chmod 600 "$TEMP"
mv -f "$TEMP" "$TARGET"
trap - EXIT
# Defaults do not replace values already saved in sdkconfig. Update only the
# pairing symbols there so rotation actually takes effect, preserving Wi-Fi
# and bridge settings. Never print configuration contents.
python3 "$(dirname "$0")/update-wireless-sdkconfig.py" "$TARGET" "$FIRMWARE_DIR/sdkconfig"
printf 'Provisioned %s for board %s. The generated file is mode 600; it contains private pairing material.\n' "$TARGET" "$BOARD_ID"
printf 'Build with: cd %s && SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.wireless.defaults" idf.py reconfigure build\n' "$FIRMWARE_DIR"
