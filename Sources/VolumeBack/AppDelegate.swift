import AppKit
import CoreAudio
import ServiceManagement

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {

    private let engine = VolumeEngine()

    private var statusItem: NSStatusItem!
    private let menu = NSMenu()

    private let statusInfoItem = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let deviceMenuItem = NSMenuItem(title: "Output Device", action: nil, keyEquivalent: "")
    private let deviceSubmenu = NSMenu()
    private let installDriverItem = NSMenuItem(
        title: "Install Audio Driver…",
        action: #selector(installDriver),
        keyEquivalent: ""
    )
    private let loginItem = NSMenuItem(title: "Launch at Login", action: #selector(toggleLogin), keyEquivalent: "")

    private var driverInstallPromptShown = false
    private var driverVersionChecked = false

    private var installedDriverURL: URL {
        URL(fileURLWithPath: "/Library/Audio/Plug-Ins/HAL/VolumeBack.driver")
    }
    private var bundledDriverURL: URL? {
        Bundle.main.resourceURL?.appendingPathComponent("VolumeBack.driver")
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildStatusItem()
        buildMenu()
        engine.onStateChange = { [weak self] in
            self?.refreshUI()
            self?.handleDriverState()
        }
        engine.start()
        refreshUI()
        handleDriverState()
    }

    func applicationWillTerminate(_ notification: Notification) {
        engine.shutdown()
    }

    // MARK: - UI setup

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

        let quit = NSMenuItem(title: "Quit VolumeBack", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        menu.addItem(quit)
    }

    func menuWillOpen(_ menu: NSMenu) {
        refreshUI()
        rebuildDeviceSubmenu()
    }

    private func refreshUI() {
        switch engine.mode {
        case .active:
            statusInfoItem.title = "Controlling: \(engine.targetName)"
        case .bypassed:
            statusInfoItem.title = "Inactive – current device has its own volume control"
        case .driverMissing:
            statusInfoItem.title = "Audio driver not installed"
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
                item.toolTip = "This device has its own volume control – VolumeBack is not needed here."
            }
            deviceSubmenu.addItem(item)
        }
    }

    // MARK: - Driver install flow

    /// Zeigt beim ersten Start (oder nach einem Treiber-Update in der App)
    /// proaktiv einen Installations-Dialog, statt die Funktion im Menue zu
    /// verstecken.
    private func handleDriverState() {
        switch engine.mode {
        case .driverMissing:
            guard !driverInstallPromptShown else { return }
            driverInstallPromptShown = true
            promptDriverInstall(isUpdate: false)
        case .active, .bypassed:
            checkDriverVersionOnce()
        case .error:
            break
        }
    }

    /// Vergleicht die Version des installierten Treibers mit der in der App
    /// mitgelieferten und bietet bei Abweichung ein Update an.
    private func checkDriverVersionOnce() {
        guard !driverVersionChecked else { return }
        driverVersionChecked = true
        guard let bundledURL = bundledDriverURL,
              let bundled = driverVersion(at: bundledURL),
              let installed = driverVersion(at: installedDriverURL),
              bundled != installed else { return }
        promptDriverInstall(isUpdate: true)
    }

    private func driverVersion(at driverURL: URL) -> String? {
        let plistURL = driverURL.appendingPathComponent("Contents/Info.plist")
        guard let data = try? Data(contentsOf: plistURL),
              let plist = try? PropertyListSerialization.propertyList(from: data, format: nil),
              let dict = plist as? [String: Any] else { return nil }
        return dict["CFBundleVersion"] as? String
    }

    private func promptDriverInstall(isUpdate: Bool) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        if isUpdate {
            alert.messageText = "Update VolumeBack Audio Driver"
            alert.informativeText = """
            This version of VolumeBack includes an updated audio driver. \
            macOS will ask for your administrator password, and audio will restart briefly.
            """
        } else {
            alert.messageText = "Install VolumeBack Audio Driver"
            alert.informativeText = """
            VolumeBack needs a small virtual audio driver to provide the native macOS \
            volume control for devices that don't have one.

            macOS will ask for your administrator password, and audio will restart briefly.
            """
        }
        alert.addButton(withTitle: isUpdate ? "Update Driver" : "Install Driver")
        alert.addButton(withTitle: "Later")
        if alert.runModal() == .alertFirstButtonReturn {
            installDriver()
        }
    }

    // MARK: - Actions

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
        guard let driverURL = bundledDriverURL,
              FileManager.default.fileExists(atPath: driverURL.path) else {
            NSLog("Bundled driver not found in app resources")
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
                NSLog("Driver installation failed: \(error)")
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
            NSLog("Login item error: \(error)")
        }
        refreshUI()
    }
}
