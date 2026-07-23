# VolumeBack

Brings the **native macOS volume control** back to devices that don't have
one (HDMI/DisplayPort monitors, some DACs). The menu bar slider, Control
Center, volume keys (F11/F12) and the system volume HUD all work as usual —
VolumeBack deliberately has no volume UI of its own.

## Download & Install

1. Grab `VolumeBack.zip` from the
   [latest release](https://github.com/AllUFknNbs/VolumeBack/releases/latest),
   unzip it and move `VolumeBack.app` to `/Applications`.
2. **First launch:** VolumeBack is open source and not notarized by Apple
   (that requires a paid developer account), so macOS will block the first
   start. Either:
   - open it once, then go to **System Settings → Privacy & Security** and
     click **"Open Anyway"**, or
   - remove the quarantine flag in Terminal:
     `xattr -d com.apple.quarantine /Applications/VolumeBack.app`
3. VolumeBack detects that its audio driver is missing and offers to
   install it (asks for your administrator password once; audio restarts
   briefly).
4. Allow **System Audio Recording** when macOS asks for it.
5. Done. Select your monitor as output (or pick it in the VolumeBack menu)
   and use volume keys / the menu bar slider as with any normal device.

Requires macOS 15 or later. Universal binary (Apple Silicon + Intel).

## How it works

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
- The virtual device shows up as "VolumeBack (Your Device)" in the native
  volume controls, so it's always clear what is being controlled.
- The app compares the installed driver version with the one bundled in the
  app and offers an update when they differ.
- Built-in updater: checks GitHub for new releases (automatically on launch
  and daily — can be turned off in the menu, manual check always available)
  and updates itself in place.

## Building from source

```sh
./build.sh                 # builds driver + app into ./build
open build/VolumeBack.app  # launch the app
```

Requires Xcode Command Line Tools. The driver can also be installed
manually:

```sh
sudo cp -R build/VolumeBack.driver /Library/Audio/Plug-Ins/HAL/
sudo killall coreaudiod
```

**Note for development:** the bundle is ad-hoc signed; after every rebuild
macOS will ask for the System Audio Recording permission again.

## Known limitations

- Sound settings show "VolumeBack" as the selected output device, not the
  monitor (inherent to this approach). The real target device is chosen in
  the VolumeBack menu.
- If the app dies hard (crash), the virtual device stays the default and
  audio is silent until the app runs again or you switch devices manually.
  → Enable "Launch at Login".
- Slight additional latency (~10 ms) from the tap round trip.

## Uninstall

```sh
sudo rm -rf /Library/Audio/Plug-Ins/HAL/VolumeBack.driver
sudo killall coreaudiod
```

Then delete `VolumeBack.app`.

## License

[MIT](LICENSE)
