#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf 'Usage: %s --host <mac-lan-host> --output <bundle.json> [--board-id ID] [--server-name NAME] [--port PORT]\n' "$0" >&2
  exit 2
}

BOARD_ID="CESP32VOICE01"
HOST=""
SERVER_NAME=""
PORT="5181"
OUTPUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --board-id) [[ $# -ge 2 ]] || usage; BOARD_ID="$2"; shift 2 ;;
    --host) [[ $# -ge 2 ]] || usage; HOST="$2"; shift 2 ;;
    --server-name) [[ $# -ge 2 ]] || usage; SERVER_NAME="$2"; shift 2 ;;
    --port) [[ $# -ge 2 ]] || usage; PORT="$2"; shift 2 ;;
    --output) [[ $# -ge 2 ]] || usage; OUTPUT="$2"; shift 2 ;;
    *) usage ;;
  esac
done

[[ -n "$HOST" && -n "$OUTPUT" ]] || usage
[[ "$PORT" =~ ^[0-9]+$ && "$PORT" -ge 1 && "$PORT" -le 65535 ]] || {
  printf 'Port must be between 1 and 65535.\n' >&2
  exit 2
}
[[ "$BOARD_ID" =~ ^[A-Za-z0-9._:-]{1,96}$ ]] || {
  printf 'Board ID contains unsupported characters.\n' >&2
  exit 2
}
[[ "$HOST" != *[/[:space:]]* ]] || {
  printf 'Host must be a DNS name or IP address, without a URL path.\n' >&2
  exit 2
}
if [[ -z "$SERVER_NAME" ]]; then SERVER_NAME="$HOST"; fi
[[ "$SERVER_NAME" != *[/[:space:]]* ]] || {
  printf 'Server name must not contain a path or whitespace.\n' >&2
  exit 2
}
# Keep endpoint/SAN fields free of URL delimiters and OpenSSL config syntax.
# This provisioning workflow supports DNS names and IPv4 addresses.
for endpoint_name in "$HOST" "$SERVER_NAME"; do
  [[ "$endpoint_name" =~ ^[A-Za-z0-9]([A-Za-z0-9.-]{0,253}[A-Za-z0-9])?$ ]] || {
    printf 'Host and TLS server name must be DNS names or IPv4 addresses.\n' >&2
    exit 2
  }
done
command -v openssl >/dev/null || { printf 'openssl is required.\n' >&2; exit 1; }
command -v jq >/dev/null || { printf 'jq is required.\n' >&2; exit 1; }

OUTPUT_DIR="$(dirname "$OUTPUT")"
mkdir -p "$OUTPUT_DIR"
umask 077
TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/codex-wireless-pairing.XXXXXX")"
cleanup() { rm -rf "$TEMP_DIR"; }
trap cleanup EXIT

KEY="$TEMP_DIR/server.key"
CERT="$TEMP_DIR/server.crt"
PKCS12="$TEMP_DIR/server.p12"
if [[ "$SERVER_NAME" =~ ^[0-9]+(\.[0-9]+){3}$ ]]; then
  SAN="IP:$SERVER_NAME"
else
  SAN="DNS:$SERVER_NAME"
fi

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 825 \
  -subj "/CN=$SERVER_NAME" \
  -addext "subjectAltName=$SAN" \
  -keyout "$KEY" -out "$CERT" >/dev/null 2>&1

CREDENTIAL="$(openssl rand -hex 32)"
PASSPHRASE="$(openssl rand -hex 24)"
openssl pkcs12 -export -out "$PKCS12" -inkey "$KEY" -in "$CERT" \
  -name "Codex ESP32 Display Wireless Microphone" \
  -passout "pass:$PASSPHRASE" >/dev/null 2>&1

IDENTITY_B64="$(base64 < "$PKCS12" | tr -d '\n')"
jq -n \
  --arg boardID "$BOARD_ID" \
  --arg host "$HOST" \
  --argjson port "$PORT" \
  --arg serverName "$SERVER_NAME" \
  --rawfile certificate "$CERT" \
  --arg credential "$CREDENTIAL" \
  --arg identity "$IDENTITY_B64" \
  --arg passphrase "$PASSPHRASE" \
  '{boardID:$boardID,host:$host,port:$port,serverName:$serverName,
    serverCertificatePEM:$certificate,credential:$credential,
    serverIdentityPKCS12Base64:$identity,serverIdentityPassphrase:$passphrase}' \
  > "$OUTPUT"
chmod 600 "$OUTPUT"
printf 'Created wireless pairing bundle at %s. Keep it private; it contains a TLS private key and board credential.\n' "$OUTPUT"
