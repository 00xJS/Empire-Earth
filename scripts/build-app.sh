#!/usr/bin/env bash
# Build Empire Earth.app and bundle the Wine helper scripts.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/dist/Empire Earth.app"
SRC="$ROOT/macos/EmpireEarthLauncher"
SDK="$(xcrun --show-sdk-path)"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/scripts"

cp "$ROOT/scripts/"*.sh "$APP/Contents/Resources/scripts/"
if compgen -G "$ROOT/scripts/*.py" >/dev/null; then
  cp "$ROOT/scripts/"*.py "$APP/Contents/Resources/scripts/"
fi
chmod +x "$APP/Contents/Resources/scripts/"*.sh
mkdir -p "$APP/Contents/Resources/patches/win32" \
  "$APP/Contents/Resources/patches/vulkan" \
  "$APP/Contents/Resources/patches/dxvk" \
  "$APP/Contents/Resources/patches/wine" \
  "$APP/Contents/Resources/patches/macos"
cp "$ROOT/patches/win32/"* "$APP/Contents/Resources/patches/win32/" 2>/dev/null || true
cp "$ROOT/patches/vulkan/"* "$APP/Contents/Resources/patches/vulkan/" 2>/dev/null || true
cp "$ROOT/patches/dxvk/"* "$APP/Contents/Resources/patches/dxvk/" 2>/dev/null || true
cp "$ROOT/patches/wine/"* "$APP/Contents/Resources/patches/wine/" 2>/dev/null || true
cp "$ROOT/patches/macos/"* "$APP/Contents/Resources/patches/macos/" 2>/dev/null || true

xcrun swiftc -parse-as-library -O \
  -strict-concurrency=minimal \
  -target arm64-apple-macos14.0 \
  -sdk "$SDK" \
  -framework SwiftUI -framework AppKit \
  "$SRC/EmpireEarthLauncherApp.swift" \
  "$SRC/LauncherModel.swift" \
  "$SRC/ContentView.swift" \
  -o "$APP/Contents/MacOS/Empire Earth"

cp "$SRC/Info.plist" "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"

echo "Built $APP"
