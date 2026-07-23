import AppKit
import CoreAudio
import ServiceManagement

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {

    private let engine = VolumeEngine()

    private var statusItem: NSStatusItem!
    private let menu = NSMenu()

    private let statusInfoItem = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let deviceMenuItem = NSMenuItem(title: "Ausgabegerät", action: nil, keyEquivalent: "")
    private let deviceSubmenu = NSMenu()
    private let installDriverItem = NSMenuItem(
        title: "Audio-Treiber installieren…",
        action: #selector(installDriver),
        keyEquivalent: ""
    )
    private let loginItem = NSMenuItem(title: "Beim Anmelden starten", action: #selector(toggleLogin), keyEquivalent: "")

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildStatusItem()
        buildMenu()
        engine.onStateChange = { [weak self] in self?.refreshUI() }
        engine.start()
        refreshUI()
    }

    func applicationWillTerminate(_ notification: Notification) {
        engine.shutdown()
    }

    // MARK: - UI-Aufbau

    private func buildStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        statusItem.button?.image = NSImage(
            systemSymbolName: "speaker.wave.2.circle",
            accessibilityDescription: "VolumeBack"
        )
        statusItem.menu = menu
    }

    private func buildMenu() {
        menu.delegate = self

        statusInfoItem.isEnabled = false
        menu.addItem(statusInfoItem)

        deviceMenuItem.submenu = deviceSubmenu
        menu.addItem(deviceMenuItem)

        installDriverItem.target = self
        menu.addItem(installDriverItem)

        menu.addItem(.separator())

        loginItem.target = self
        menu.addItem(loginItem)

        menu.addItem(.separator())

        let quit = NSMenuItem(title: "VolumeBack beenden", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        menu.addItem(quit)
    }

    func menuWillOpen(_ menu: NSMenu) {
        refreshUI()
        rebuildDeviceSubmenu()
    }

    private func refreshUI() {
        switch engine.mode {
        case .active:
            statusInfoItem.title = "Regelt: \(engine.targetName)"
        case .bypassed:
            statusInfoItem.title = "Inaktiv – aktuelles Gerät regelt selbst"
        case .driverMissing:
            statusInfoItem.title = "Treiber nicht installiert"
        case .error(let message):
            statusInfoItem.title = message
        }

        installDriverItem.isHidden = engine.mode != .driverMissing
        deviceMenuItem.isHidden = engine.mode == .driverMissing
        loginItem.state = SMAppService.mainApp.status == .enabled ? .on : .off

        let symbol = engine.mode == .active ? "speaker.wave.2.circle.fill" : "speaker.wave.2.circle"
        statusItem.button?.image = NSImage(systemSymbolName: symbol, accessibilityDescription: "VolumeBack")
    }

    private func rebuildDeviceSubmenu() {
        deviceSubmenu.removeAllItems()
        for deviceID in CA.outputDevices() {
            guard deviceID != engine.aggregateID else { continue }
            guard let uid = CA.deviceUID(deviceID), uid != VolumeEngine.virtualUID else { continue }
            let item = NSMenuItem(title: CA.deviceName(deviceID), action: #selector(selectDevice(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = NSNumber(value: deviceID)
            item.state = (uid == engine.targetUID && engine.mode == .active) ? .on : .off
            if CA.hasSettableVolume(deviceID) {
                item.toolTip = "Gerät hat eine eigene Lautstärkeregelung – VolumeBack ist hier nicht nötig, funktioniert aber."
            }
            deviceSubmenu.addItem(item)
        }
    }

    // MARK: - Aktionen

    @objc private func selectDevice(_ sender: NSMenuItem) {
        guard let number = sender.representedObject as? NSNumber else { return }
        let deviceID = AudioObjectID(number.uint32Value)
        if CA.hasSettableVolume(deviceID) {
            // Geraet regelt selbst: einfach direkt als Standard setzen.
            CA.setDefaultOutput(deviceID)
        } else {
            engine.engage(targetDeviceID: deviceID)
        }
    }

    @objc private func installDriver() {
        guard let driverURL = Bundle.main.resourceURL?.appendingPathComponent("VolumeBack.driver"),
              FileManager.default.fileExists(atPath: driverURL.path) else {
            NSLog("Treiber nicht im App-Bundle gefunden")
            return
        }
        let script = """
        do shell script "rm -rf /Library/Audio/Plug-Ins/HAL/VolumeBack.driver && \
        cp -R '\(driverURL.path)' /Library/Audio/Plug-Ins/HAL/ && \
        killall coreaudiod" with administrator privileges
        """
        DispatchQueue.global().async {
            var error: NSDictionary?
            NSAppleScript(source: script)?.executeAndReturnError(&error)
            if let error {
                NSLog("Treiber-Installation fehlgeschlagen: \(error)")
            }
            // coreaudiod-Neustart feuert die Geraete-Listener der Engine,
            // die Pipeline baut sich dann von selbst auf.
        }
    }

    @objc private func toggleLogin() {
        do {
            if SMAppService.mainApp.status == .enabled {
                try SMAppService.mainApp.unregister()
            } else {
                try SMAppService.mainApp.register()
            }
        } catch {
            NSLog("Login-Item-Fehler: \(error)")
        }
        refreshUI()
    }
}
