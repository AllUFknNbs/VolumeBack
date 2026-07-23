#!/bin/zsh
# Baut VolumeBack.app (inkl. HAL-Treiber in den App-Ressourcen) nach ./build
set -euo pipefail
cd "$(dirname "$0")"

# 1. HAL-Treiber
DRIVER="build/VolumeBack.driver"
rm -rf "$DRIVER"
mkdir -p "$DRIVER/Contents/MacOS"
clang -bundle -O2 \
    -o "$DRIVER/Contents/MacOS/VolumeBack" \
    Driver/VolumeBackDriver.c \
    -framework CoreFoundation -framework CoreAudio
cp Driver/Info.plist "$DRIVER/Contents/Info.plist"
codesign --force --sign - "$DRIVER"

# 2. App
swift build -c release

APP="build/VolumeBack.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/VolumeBack "$APP/Contents/MacOS/VolumeBack"
cp Resources/Info.plist "$APP/Contents/Info.plist"
cp -R "$DRIVER" "$APP/Contents/Resources/VolumeBack.driver"

# Ad-hoc-Signatur (noetig fuer TCC-Berechtigungen wie Systemaudio-Aufnahme)
codesign --force --sign - "$APP"

echo "Fertig: $APP"
echo "Starten mit: open '$APP'"
