#!/bin/zsh
# Builds VolumeBack.app (universal, with the HAL driver embedded in the app
# resources) into ./build
set -euo pipefail
cd "$(dirname "$0")"

# 1. HAL driver (universal)
DRIVER="build/VolumeBack.driver"
rm -rf "$DRIVER"
mkdir -p "$DRIVER/Contents/MacOS"
clang -bundle -O2 \
    -arch arm64 -arch x86_64 \
    -o "$DRIVER/Contents/MacOS/VolumeBack" \
    Driver/VolumeBackDriver.c \
    -framework CoreFoundation -framework CoreAudio
cp Driver/Info.plist "$DRIVER/Contents/Info.plist"
codesign --force --sign - "$DRIVER"

# 2. App (universal; built per-arch and merged, works without full Xcode)
swift build -c release --triple arm64-apple-macosx
swift build -c release --triple x86_64-apple-macosx
BINARY="build/VolumeBack-universal"
mkdir -p build
lipo -create \
    .build/arm64-apple-macosx/release/VolumeBack \
    .build/x86_64-apple-macosx/release/VolumeBack \
    -output "$BINARY"

APP="build/VolumeBack.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BINARY" "$APP/Contents/MacOS/VolumeBack"
cp Resources/Info.plist "$APP/Contents/Info.plist"
cp -R "$DRIVER" "$APP/Contents/Resources/VolumeBack.driver"

# Ad-hoc signature (required for TCC permissions such as System Audio Recording)
codesign --force --sign - "$APP"

echo "Done: $APP"
echo "Run with: open '$APP'"
