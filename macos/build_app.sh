#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MACOS_ROOT="$ROOT/macos"
APP="$MACOS_ROOT/build/Codex ESP32 Display.app"
SIGN_IDENTITY="${CODEX_DISPLAY_SIGN_IDENTITY:-}"
if [[ -z "$SIGN_IDENTITY" && -f "$MACOS_ROOT/.signing-identity" ]]; then
  SIGN_IDENTITY="$(cat "$MACOS_ROOT/.signing-identity")"
fi
SIGN_IDENTITY="${SIGN_IDENTITY:--}"

swift build --package-path "$MACOS_ROOT" --configuration release --product CodexESP32Display
BIN_DIR="$(swift build --package-path "$MACOS_ROOT" --configuration release --show-bin-path)"

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN_DIR/CodexESP32Display" "$APP/Contents/MacOS/CodexESP32Display"
cp "$MACOS_ROOT/Info.plist" "$APP/Contents/Info.plist"

# Generate every application icon size from the menu bar's shared device shape.
swiftc "$MACOS_ROOT/Sources/CodexESP32Display/DeviceOutlineIcon.swift" \
  "$MACOS_ROOT/scripts/GenerateIcons.swift" -o "$MACOS_ROOT/build/generate-icons"
"$MACOS_ROOT/build/generate-icons" "$MACOS_ROOT/build/DeviceIcon.iconset"
iconutil -c icns "$MACOS_ROOT/build/DeviceIcon.iconset" \
  -o "$APP/Contents/Resources/DeviceIcon.icns"

rm -rf "$APP/Contents/Resources/bridge"
mkdir -p "$APP/Contents/Resources/bridge"
ditto "$ROOT/bridge/src" "$APP/Contents/Resources/bridge/src"
chmod +x "$APP/Contents/MacOS/CodexESP32Display"

if [[ "$SIGN_IDENTITY" != "-" ]]; then
  # Developer ID distribution requires the hardened runtime for notarization.
  codesign --force --deep --options runtime --timestamp --sign "$SIGN_IDENTITY" "$APP" >/dev/null
else
  codesign --force --deep --sign "$SIGN_IDENTITY" "$APP" >/dev/null
fi
codesign --verify --deep --strict "$APP"
if [[ "$SIGN_IDENTITY" == "-" ]]; then
  printf 'Ad hoc signature: macOS privacy permissions may need reapproval after a rebuild.\n' >&2
fi
printf 'Built %s\n' "$APP"
