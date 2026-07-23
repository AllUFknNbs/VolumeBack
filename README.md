# VolumeBack

Brings the **native macOS volume control** back to devices that don't have
one (HDMI/DisplayPort monitors, some DACs). The menu bar slider, Control
Center, F11/F12 keys and the system volume HUD all work as usual —
VolumeBack deliberately has no volume UI of its own.

## Architecture

Two parts:

1. **HAL driver** (`Driver/VolumeBackDriver.c`): a minimal virtual output
   device ("VolumeBack") whose only purpose is to host a native volume and
   mute control. It discards all audio (null sink). No kext — just a regular
   AudioServerPlugIn in `/Library/Audio/Plug-Ins/HAL/`.

2. **Menu bar app** (`Sources/VolumeBack/`): sets the virtual device as the
   default output, captures the system audio played to it via a
   **Core Audio process tap** (macOS 14.4+) and plays it back on the real
   target device, applying the native control's value as gain.

To macOS the virtual device looks like a perfectly normal device with a
volume control — which is why the entire native volume UI just works.

Behavior:
- When a device **without** its own volume control (a monitor) becomes the
  default output, VolumeBack takes over automatically.
- When a device **with** its own control (AirPods, built-in speakers) is
  selected, VolumeBack stays completely out of the way.
- Volume is remembered per target device; on quit the app restores the real
  device as the default output.

## Building & Installing

```sh
./build.sh                 # builds driver + app into ./build
open build/VolumeBack.app  # launch the app
```

Install the driver: menu bar icon → "Audio-Treiber installieren…"
(or manually:)

```sh
sudo cp -R build/VolumeBack.driver /Library/Audio/Plug-Ins/HAL/
sudo killall coreaudiod
```

Requires: macOS 15+, Xcode Command Line Tools.

## Permissions

- **System Audio Recording** (required, for the process tap): macOS asks on
  first activation. Until granted, the app retries every few seconds.
- Nothing else. No Accessibility permission needed — the volume keys work
  natively through the virtual device.

**Note:** the bundle is ad-hoc signed; after every rebuild macOS will ask
for the permission again.

## Known limitations

- Sound settings show "VolumeBack" as the selected output device, not the
  monitor (inherent to this approach). The real target device is chosen in
  the VolumeBack menu.
- If the app dies hard (crash), the virtual device stays the default and
  audio is silent until the app runs again or you switch devices manually.
  → Enable "Beim Anmelden starten" (launch at login).
- Slight additional latency (~10 ms) from the tap round trip.

## Uninstall

```sh
sudo rm -rf /Library/Audio/Plug-Ins/HAL/VolumeBack.driver
sudo killall coreaudiod
```
