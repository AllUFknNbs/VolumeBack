import AppKit
import Foundation

/// Prueft GitHub-Releases auf neue Versionen und kann die App selbst
/// aktualisieren (Zip laden, Bundle ersetzen, neu starten).
///
/// Auto-Check (Start + alle 24 h) ist per Menue abschaltbar; der manuelle
/// Check steht immer zur Verfuegung.
final class UpdateChecker {

    private static let repo = "AllUFknNbs/VolumeBack"
    private static let autoCheckKey = "autoUpdateCheck"
    private static let releasesPage = URL(string: "https://github.com/AllUFknNbs/VolumeBack/releases/latest")!

    private var timer: Timer?
    private var updateInProgress = false

    var currentVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    }

    var autoCheckEnabled: Bool {
        get { UserDefaults.standard.object(forKey: Self.autoCheckKey) as? Bool ?? true }
        set {
            UserDefaults.standard.set(newValue, forKey: Self.autoCheckKey)
            newValue ? startAutoCheck() : stopAutoCheck()
        }
    }

    func startIfEnabled() {
        if autoCheckEnabled { startAutoCheck() }
    }

    private func startAutoCheck() {
        stopAutoCheck()
        DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self] in
            self?.check(userInitiated: false)
        }
        timer = Timer.scheduledTimer(withTimeInterval: 24 * 3600, repeats: true) { [weak self] _ in
            self?.check(userInitiated: false)
        }
    }

    private func stopAutoCheck() {
        timer?.invalidate()
        timer = nil
    }

    // MARK: - Check

    private struct Release {
        let version: String
        let zipURL: URL
        let pageURL: URL
    }

    func check(userInitiated: Bool) {
        guard !updateInProgress else { return }
        var request = URLRequest(url: URL(string: "https://api.github.com/repos/\(Self.repo)/releases/latest")!)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.timeoutInterval = 15

        URLSession.shared.dataTask(with: request) { [weak self] data, _, error in
            DispatchQueue.main.async {
                guard let self else { return }
                guard let data,
                      let object = try? JSONSerialization.jsonObject(with: data),
                      let json = object as? [String: Any],
                      let tag = json["tag_name"] as? String else {
                    if userInitiated {
                        self.showAlert(
                            title: "Update Check Failed",
                            text: error?.localizedDescription ?? "Could not reach GitHub."
                        )
                    }
                    return
                }

                let latest = tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
                let assets = json["assets"] as? [[String: Any]] ?? []
                let zipAsset = assets.first { ($0["name"] as? String)?.hasSuffix(".zip") == true }
                let zipURLString = zipAsset?["browser_download_url"] as? String
                let pageURL = (json["html_url"] as? String).flatMap(URL.init(string:)) ?? Self.releasesPage

                if Self.isVersion(latest, newerThan: self.currentVersion),
                   let zipURLString, let zipURL = URL(string: zipURLString) {
                    self.offerUpdate(Release(version: latest, zipURL: zipURL, pageURL: pageURL))
                } else if userInitiated {
                    self.showAlert(
                        title: "You're up to date",
                        text: "VolumeBack \(self.currentVersion) is the latest version."
                    )
                }
            }
        }.resume()
    }

    static func isVersion(_ a: String, newerThan b: String) -> Bool {
        let partsA = a.split(separator: ".").map { Int($0) ?? 0 }
        let partsB = b.split(separator: ".").map { Int($0) ?? 0 }
        for index in 0..<max(partsA.count, partsB.count) {
            let x = index < partsA.count ? partsA[index] : 0
            let y = index < partsB.count ? partsB[index] : 0
            if x != y { return x > y }
        }
        return false
    }

    // MARK: - Update-Dialog & Installation

    private func offerUpdate(_ release: Release) {
        guard !updateInProgress else { return }
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "VolumeBack \(release.version) is available"
        alert.informativeText = """
        You are running \(currentVersion). Do you want to install the update now? \
        The app will restart automatically.

        Note: macOS will ask for the System Audio Recording permission again after updating.
        """
        alert.addButton(withTitle: "Install & Relaunch")
        alert.addButton(withTitle: "Show Release Notes")
        alert.addButton(withTitle: "Later")
        switch alert.runModal() {
        case .alertFirstButtonReturn:
            downloadAndInstall(release)
        case .alertSecondButtonReturn:
            NSWorkspace.shared.open(release.pageURL)
        default:
            break
        }
    }

    private func downloadAndInstall(_ release: Release) {
        updateInProgress = true
        URLSession.shared.downloadTask(with: release.zipURL) { [weak self] tempURL, _, error in
            DispatchQueue.main.async {
                guard let self else { return }
                guard let tempURL else {
                    self.updateInProgress = false
                    self.showInstallError(error?.localizedDescription ?? "Download failed.")
                    return
                }
                // downloadTask loescht die Temp-Datei nach Rueckkehr — vorher wegkopieren.
                let zipCopy = FileManager.default.temporaryDirectory
                    .appendingPathComponent("VolumeBack-\(release.version).zip")
                try? FileManager.default.removeItem(at: zipCopy)
                do {
                    try FileManager.default.copyItem(at: tempURL, to: zipCopy)
                    try self.install(zipAt: zipCopy)
                } catch {
                    self.updateInProgress = false
                    self.showInstallError(error.localizedDescription)
                }
            }
        }.resume()
    }

    private struct UpdateError: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    private func install(zipAt zipURL: URL) throws {
        let fm = FileManager.default
        let workDir = fm.temporaryDirectory.appendingPathComponent("VolumeBackUpdate-\(UUID().uuidString)")
        try fm.createDirectory(at: workDir, withIntermediateDirectories: true)

        let unzip = Process()
        unzip.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        unzip.arguments = ["-xk", zipURL.path, workDir.path]
        try unzip.run()
        unzip.waitUntilExit()
        guard unzip.terminationStatus == 0 else {
            throw UpdateError(message: "Could not unpack the update archive.")
        }

        let newApp = workDir.appendingPathComponent("VolumeBack.app")
        guard fm.fileExists(atPath: newApp.path) else {
            throw UpdateError(message: "The update archive does not contain VolumeBack.app.")
        }

        let destination = Bundle.main.bundleURL
        let backup = fm.temporaryDirectory.appendingPathComponent("VolumeBack-old-\(UUID().uuidString).app")
        try fm.moveItem(at: destination, to: backup)
        do {
            try fm.moveItem(at: newApp, to: destination)
        } catch {
            // Altes Bundle zurueckstellen, sonst ist die App weg.
            try? fm.moveItem(at: backup, to: destination)
            throw error
        }

        relaunch(at: destination)
    }

    private func relaunch(at appURL: URL) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = ["-c", "sleep 1; /usr/bin/open \"\(appURL.path)\""]
        try? process.run()
        NSApp.terminate(nil)
    }

    // MARK: - Alerts

    private func showInstallError(_ message: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Update Failed"
        alert.informativeText = "\(message)\n\nYou can download the update manually from the releases page."
        alert.addButton(withTitle: "Open Releases Page")
        alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn {
            NSWorkspace.shared.open(Self.releasesPage)
        }
    }

    private func showAlert(title: String, text: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        alert.runModal()
    }
}
