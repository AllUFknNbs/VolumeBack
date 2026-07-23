# VolumeBack

Bringt den **nativen macOS-Lautstärkeregler** für Geräte zurück, die keinen
haben (HDMI-/DisplayPort-Monitore, manche DACs). Menüleisten-Slider,
Control Center, F11/F12 und das System-HUD funktionieren ganz normal —
VolumeBack hat bewusst keinerlei eigene Regler-UI.

## Architektur

Zwei Teile:

1. **HAL-Treiber** (`Driver/VolumeBackDriver.c`): ein minimales virtuelles
   Ausgabegerät („VolumeBack"), dessen einziger Zweck ein nativer
   Lautstärke- und Mute-Regler ist. Es verwirft alles Audio (Null-Sink).
   Kein Kext — ein normales AudioServerPlugIn in
   `/Library/Audio/Plug-Ins/HAL/`.

2. **Menüleisten-App** (`Sources/VolumeBack/`): setzt das virtuelle Gerät als
   Standardausgang, greift das dorthin gespielte System-Audio per
   **Core Audio Process Tap** (macOS 14.4+) ab und spielt es mit Gain
   (= Wert des nativen Reglers) auf dem echten Zielgerät aus.

Für macOS sieht das virtuelle Gerät wie ein ganz normales Gerät mit
Lautstärkeregler aus — deshalb funktioniert die komplette native Volume-UI.

Verhalten:
- Wird ein Gerät **ohne** eigene Regelung (Monitor) zum Standard, übernimmt
  VolumeBack automatisch.
- Wird ein Gerät **mit** eigener Regelung (AirPods, interne Lautsprecher)
  gewählt, hält sich VolumeBack komplett raus.
- Lautstärke wird pro Zielgerät gespeichert; beim Beenden stellt die App das
  echte Gerät als Standard wieder her.

## Bauen & Installieren

```sh
./build.sh                 # baut Treiber + App nach ./build
open build/VolumeBack.app  # App starten
```

Treiber installieren: Menüleisten-Symbol → „Audio-Treiber installieren…"
(oder manuell:)

```sh
sudo cp -R build/VolumeBack.driver /Library/Audio/Plug-Ins/HAL/
sudo killall coreaudiod
```

Benötigt: macOS 15+, Xcode Command Line Tools.

## Berechtigungen

- **Systemaudio-Aufnahme** (Pflicht, für den Tap): macOS fragt beim ersten
  Aktivieren. Bis zur Freigabe versucht die App es alle paar Sekunden erneut.
- Sonst nichts. Keine Bedienungshilfen nötig — die Lautstärketasten laufen
  nativ über das virtuelle Gerät.

**Hinweis:** Bundle ist ad-hoc-signiert; nach jedem Neubauen fragt macOS die
Berechtigung erneut ab.

## Bekannte Grenzen

- In den Soundeinstellungen ist „VolumeBack" als Ausgabegerät ausgewählt,
  nicht der Monitor (systembedingt). Das echte Zielgerät wählt man im
  VolumeBack-Menü.
- Beendet man die App hart (Crash), bleibt das virtuelle Gerät Standard und
  es ist still, bis die App wieder läuft oder man das Gerät manuell wechselt.
  → „Beim Anmelden starten" aktivieren.
- Minimal zusätzliche Latenz (~10 ms) durch den Tap-Umweg.

## Deinstallation

```sh
sudo rm -rf /Library/Audio/Plug-Ins/HAL/VolumeBack.driver
sudo killall coreaudiod
```
