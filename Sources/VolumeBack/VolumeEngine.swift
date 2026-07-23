import AudioToolbox
import Accelerate
import CoreAudio
import Foundation

/// Kern der App.
///
/// Der VolumeBack-HAL-Treiber stellt ein virtuelles Ausgabegeraet mit nativem
/// Lautstaerke-/Mute-Regler bereit. Diese Engine setzt es als Standardgeraet,
/// greift das dorthin gespielte System-Audio per Process-Tap ab und gibt es
/// mit Gain (= nativer Reglerwert) auf dem echten Zielgeraet aus.
///
/// macOS sieht dadurch ein ganz normales Geraet mit Lautstaerkeregler:
/// Menueleisten-Slider, Control Center, F11/F12 und das System-HUD
/// funktionieren nativ — die App selbst hat keinerlei eigene Regler-UI.
final class VolumeEngine {

    static let virtualUID = "VolumeBack_Device_UID"

    enum Mode: Equatable {
        /// Pipeline laeuft: virtuelles Geraet ist Standard, Audio geht ans Zielgeraet
        case active
        /// Nutzer hat ein Geraet mit eigener Regelung gewaehlt — wir halten uns raus
        case bypassed
        /// HAL-Treiber nicht installiert
        case driverMissing
        case error(String)
    }

    private(set) var mode: Mode = .bypassed
    private(set) var virtualID = AudioObjectID(kAudioObjectUnknown)
    private(set) var targetID = AudioObjectID(kAudioObjectUnknown)
    private(set) var targetUID: String?
    private(set) var targetName = "–"

    /// Wird auf dem Main-Thread gerufen, wenn sich der Zustand aendert.
    var onStateChange: (() -> Void)?

    /// Gain-Zustand fuer den Realtime-IO-Thread. Aligned Float32-Zugriffe
    /// sind auf arm64 nicht zerreissbar, daher reicht das hier.
    private final class GainBox {
        var gain: Float = 1.0
    }
    private let gainBox = GainBox()
    private let defaults = UserDefaults.standard

    // Tap-Pipeline
    private var tapID = AudioObjectID(kAudioObjectUnknown)
    /// Unser privates Aggregat. Fuer andere Prozesse unsichtbar, aber der
    /// eigene Prozess sieht es in der Geraeteliste — die UI filtert es damit raus.
    private(set) var aggregateID = AudioObjectID(kAudioObjectUnknown)
    private var ioProcID: AudioDeviceIOProcID?
    private var ioRunning = false

    private var rebuildScheduled = false
    private var retryScheduled = false
    private var controlListenersInstalledFor = AudioObjectID(kAudioObjectUnknown)

    // MARK: - Lifecycle

    func start() {
        installGlobalListeners()
        rebuild()
    }

    /// Beim Beenden: Standardgeraet zurueck aufs echte Geraet, sonst laeuft
    /// das System-Audio ins virtuelle Nichts.
    func shutdown() {
        tearDownPipeline()
        if let current = CA.defaultOutputDevice(), current == virtualID,
           targetID != kAudioObjectUnknown {
            CA.setDefaultOutput(targetID)
        }
    }

    // MARK: - Oeffentliche Aktionen

    /// Nutzer waehlt im Menue ein Zielgeraet: Routing dorthin uebernehmen.
    func engage(targetDeviceID: AudioObjectID) {
        guard let uid = CA.deviceUID(targetDeviceID), uid != Self.virtualUID else { return }
        targetID = targetDeviceID
        targetUID = uid
        targetName = CA.deviceName(targetDeviceID)
        defaults.set(uid, forKey: "targetUID")
        if virtualID != kAudioObjectUnknown {
            CA.setDefaultOutput(virtualID)
        }
        rebuild()
    }

    // MARK: - Aufbau

    func rebuild() {
        assert(Thread.isMainThread)
        tearDownPipeline()

        guard let virtualDevice = CA.findDevice(uid: Self.virtualUID) else {
            virtualID = kAudioObjectUnknown
            mode = .driverMissing
            onStateChange?()
            return
        }
        virtualID = virtualDevice

        resolveTarget()
        guard targetID != kAudioObjectUnknown else {
            mode = .error("Kein Zielgerät gefunden")
            onStateChange?()
            return
        }

        // Ist gerade ein anderes Geraet Standard?
        if let currentDefault = CA.defaultOutputDevice(), currentDefault != virtualID {
            if CA.hasSettableVolume(currentDefault) {
                // AirPods & Co. regeln selbst — nicht einmischen.
                mode = .bypassed
                onStateChange?()
                return
            }
            // Geraet ohne eigene Regelung (Monitor): uebernehmen.
            targetID = currentDefault
            targetUID = CA.deviceUID(currentDefault)
            targetName = CA.deviceName(currentDefault)
            if let uid = targetUID { defaults.set(uid, forKey: "targetUID") }
            CA.setDefaultOutput(virtualID)
        }

        // Abtastrate des virtuellen Geraets ans Zielgeraet angleichen,
        // damit das Aggregat nicht resamplen muss.
        if let targetRate = CA.nominalSampleRate(targetID), targetRate > 0 {
            CA.set(virtualID, CA.address(kAudioDevicePropertyNominalSampleRate), value: targetRate)
        }

        do {
            try buildTapPipeline()
            mode = .active
            installControlListeners()
            pushPersistedVolumeToVirtualDevice()
            applyGainFromVirtualDevice()
        } catch {
            tearDownPipeline()
            mode = .error(error.localizedDescription)
            scheduleRetry()
        }
        onStateChange?()
    }

    private func resolveTarget() {
        if let uid = defaults.string(forKey: "targetUID"), let id = CA.findDevice(uid: uid) {
            targetID = id
        } else if let current = CA.defaultOutputDevice(), current != virtualID {
            targetID = current
        } else if let first = CA.outputDevices().first(where: { $0 != virtualID }) {
            targetID = first
        } else {
            targetID = kAudioObjectUnknown
            return
        }
        targetUID = CA.deviceUID(targetID)
        targetName = CA.deviceName(targetID)
    }

    private struct EngineError: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    private func buildTapPipeline() throws {
        guard let targetUID else { throw EngineError(message: "Zielgerät hat keine UID") }

        // 1. Tap auf das virtuelle Geraet: Audio aller Prozesse abgreifen,
        //    Original stummschalten (ohnehin ein Null-Sink). Uns selbst
        //    ausschliessen, sonst Feedback-Schleife.
        var excluded: [NSNumber] = []
        if let own = CA.ownProcessObject() {
            excluded.append(NSNumber(value: own))
        }
        // NS_REFINED_FOR_SWIFT ohne Swift-Overlay-Pendant -> __-Prefix noetig.
        let tapDescription = CATapDescription(
            __excludingProcesses: excluded,
            andDeviceUID: Self.virtualUID,
            withStream: 0
        )
        tapDescription.name = "VolumeBack System Tap"
        tapDescription.isPrivate = true
        tapDescription.muteBehavior = .mutedWhenTapped

        var newTapID = AudioObjectID(kAudioObjectUnknown)
        var err = AudioHardwareCreateProcessTap(tapDescription, &newTapID)
        guard err == noErr, newTapID != kAudioObjectUnknown else {
            throw EngineError(message: "Tap fehlgeschlagen (Fehler \(err)) – Berechtigung „Systemaudio-Aufnahme“ erteilt?")
        }
        tapID = newTapID

        // 2. Privates Aggregat: echtes Zielgeraet als Ausgang + Tap als Eingang.
        let description: [String: Any] = [
            kAudioAggregateDeviceNameKey: "VolumeBack Loopback",
            kAudioAggregateDeviceUIDKey: UUID().uuidString,
            kAudioAggregateDeviceMainSubDeviceKey: targetUID,
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceIsStackedKey: false,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceSubDeviceListKey: [
                [kAudioSubDeviceUIDKey: targetUID]
            ],
            kAudioAggregateDeviceTapListKey: [
                [
                    kAudioSubTapUIDKey: tapDescription.uuid.uuidString,
                    kAudioSubTapDriftCompensationKey: true,
                ]
            ],
        ]

        var newAggregateID = AudioObjectID(kAudioObjectUnknown)
        err = AudioHardwareCreateAggregateDevice(description as CFDictionary, &newAggregateID)
        guard err == noErr, newAggregateID != kAudioObjectUnknown else {
            throw EngineError(message: "Aggregatgerät fehlgeschlagen (Fehler \(err))")
        }
        aggregateID = newAggregateID

        // 3. IO-Proc: Tap-Eingang mit Gain auf den Geraeteausgang kopieren.
        //    Queue = nil ist Pflicht: der Block laeuft dann direkt auf dem
        //    Realtime-IO-Thread der HAL. Mit einer Dispatch-Queue kommt
        //    Scheduling-Jitter rein -> Aussetzer.
        let box = gainBox
        err = AudioDeviceCreateIOProcIDWithBlock(&ioProcID, aggregateID, nil) { _, inInputData, _, outOutputData, _ in
            VolumeEngine.render(input: inInputData, output: outOutputData, gain: box.gain)
        }
        guard err == noErr, ioProcID != nil else {
            throw EngineError(message: "IO-Proc fehlgeschlagen (Fehler \(err))")
        }

        err = AudioDeviceStart(aggregateID, ioProcID)
        guard err == noErr else {
            throw EngineError(message: "Audio-IO-Start fehlgeschlagen (Fehler \(err))")
        }
        ioRunning = true
    }

    private func tearDownPipeline() {
        if let proc = ioProcID, aggregateID != kAudioObjectUnknown {
            if ioRunning { AudioDeviceStop(aggregateID, proc) }
            AudioDeviceDestroyIOProcID(aggregateID, proc)
        }
        ioProcID = nil
        ioRunning = false

        if aggregateID != kAudioObjectUnknown {
            AudioHardwareDestroyAggregateDevice(aggregateID)
            aggregateID = kAudioObjectUnknown
        }
        if tapID != kAudioObjectUnknown {
            AudioHardwareDestroyProcessTap(tapID)
            tapID = kAudioObjectUnknown
        }
    }

    // MARK: - Lautstaerke-Sync (nativer Regler -> Gain)

    private func readVirtualVolume() -> (scalar: Float, muted: Bool) {
        let scalar = CA.get(
            virtualID,
            CA.address(kAudioDevicePropertyVolumeScalar, scope: kAudioDevicePropertyScopeOutput),
            defaultValue: Float(0.75)
        ) ?? 0.75
        let mute = CA.get(
            virtualID,
            CA.address(kAudioDevicePropertyMute, scope: kAudioDevicePropertyScopeOutput),
            defaultValue: UInt32(0)
        ) ?? 0
        return (scalar, mute != 0)
    }

    private func applyGainFromVirtualDevice() {
        let (scalar, muted) = readVirtualVolume()
        // Quadratische Kurve: fuehlt sich wie ein normaler macOS-Regler an.
        gainBox.gain = muted ? 0 : scalar * scalar
        if let uid = targetUID {
            defaults.set(scalar, forKey: "volume.\(uid)")
            defaults.set(muted, forKey: "muted.\(uid)")
        }
    }

    private func pushPersistedVolumeToVirtualDevice() {
        guard let uid = targetUID, defaults.object(forKey: "volume.\(uid)") != nil else { return }
        let scalar = defaults.float(forKey: "volume.\(uid)")
        let muted: UInt32 = defaults.bool(forKey: "muted.\(uid)") ? 1 : 0
        CA.set(
            virtualID,
            CA.address(kAudioDevicePropertyVolumeScalar, scope: kAudioDevicePropertyScopeOutput),
            value: scalar
        )
        CA.set(
            virtualID,
            CA.address(kAudioDevicePropertyMute, scope: kAudioDevicePropertyScopeOutput),
            value: muted
        )
    }

    /// Beobachtet den nativen Regler des virtuellen Geraets.
    private func installControlListeners() {
        guard controlListenersInstalledFor != virtualID else { return }
        controlListenersInstalledFor = virtualID

        let handler: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            guard let self else { return }
            self.applyGainFromVirtualDevice()
            self.onStateChange?()
        }
        var volumeAddr = CA.address(kAudioDevicePropertyVolumeScalar, scope: kAudioDevicePropertyScopeOutput)
        var muteAddr = CA.address(kAudioDevicePropertyMute, scope: kAudioDevicePropertyScopeOutput)
        AudioObjectAddPropertyListenerBlock(virtualID, &volumeAddr, .main, handler)
        AudioObjectAddPropertyListenerBlock(virtualID, &muteAddr, .main, handler)
    }

    // MARK: - Realtime-Rendering

    /// Läuft auf dem IO-Thread: kein Allokieren, kein Locking, kein ObjC.
    private static func render(
        input: UnsafePointer<AudioBufferList>,
        output: UnsafeMutablePointer<AudioBufferList>,
        gain: Float
    ) {
        let inputList = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: input))
        let outputList = UnsafeMutableAudioBufferListPointer(output)

        for buffer in outputList {
            if let data = buffer.mData {
                memset(data, 0, Int(buffer.mDataByteSize))
            }
        }

        guard let inBuffer = inputList.first(where: { $0.mData != nil && $0.mNumberChannels > 0 }),
              let inRaw = inBuffer.mData else { return }

        let inChannels = Int(inBuffer.mNumberChannels)
        let inFrames = Int(inBuffer.mDataByteSize) / (MemoryLayout<Float>.size * inChannels)
        let inSamples = inRaw.assumingMemoryBound(to: Float.self)

        for outBuffer in outputList {
            guard let outRaw = outBuffer.mData, outBuffer.mNumberChannels > 0 else { continue }
            let outChannels = Int(outBuffer.mNumberChannels)
            let outFrames = Int(outBuffer.mDataByteSize) / (MemoryLayout<Float>.size * outChannels)
            let outSamples = outRaw.assumingMemoryBound(to: Float.self)
            let frames = min(inFrames, outFrames)

            if outChannels == inChannels {
                var scale = gain
                vDSP_vsmul(inSamples, 1, &scale, outSamples, 1, vDSP_Length(frames * outChannels))
            } else {
                for frame in 0..<frames {
                    for channel in 0..<outChannels {
                        let source = min(channel, inChannels - 1)
                        outSamples[frame * outChannels + channel] =
                            inSamples[frame * inChannels + source] * gain
                    }
                }
            }
        }
    }

    // MARK: - Systemweite Listener

    private func installGlobalListeners() {
        let system = AudioObjectID(kAudioObjectSystemObject)
        let handler: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            self?.scheduleRebuild()
        }
        var defaultAddr = CA.address(kAudioHardwarePropertyDefaultOutputDevice)
        var devicesAddr = CA.address(kAudioHardwarePropertyDevices)
        AudioObjectAddPropertyListenerBlock(system, &defaultAddr, .main, handler)
        AudioObjectAddPropertyListenerBlock(system, &devicesAddr, .main, handler)
    }

    /// Geraetewechsel feuern oft mehrfach hintereinander — leicht entprellen.
    /// Wichtig: Unser eigener Pipeline-Aufbau (Aggregat erzeugen/zerstoeren)
    /// feuert die Geraete-Listener ebenfalls. Ohne den Steady-State-Check
    /// wuerde sich die Pipeline daher endlos selbst abreissen und neu bauen
    /// — hoerbar als regelmaessig abgehacktes Audio.
    private func scheduleRebuild() {
        guard !rebuildScheduled else { return }
        rebuildScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { [weak self] in
            guard let self else { return }
            self.rebuildScheduled = false
            if self.isInSteadyState() { return }
            self.rebuild()
        }
    }

    /// Ist der aktuelle Zustand noch gueltig? Dann keinen Rebuild ausloesen.
    private func isInSteadyState() -> Bool {
        switch mode {
        case .active:
            guard ioRunning else { return false }
            guard let virtualNow = CA.findDevice(uid: Self.virtualUID), virtualNow == virtualID else { return false }
            guard CA.defaultOutputDevice() == virtualID else { return false }
            guard let uid = targetUID, CA.findDevice(uid: uid) == targetID else { return false }
            return true
        case .bypassed:
            // Bypassed bleibt stabil, solange das Standardgeraet ein fremdes
            // Geraet mit eigener Regelung ist und der Treiber unveraendert da ist.
            guard let currentDefault = CA.defaultOutputDevice(), currentDefault != virtualID else { return false }
            guard CA.findDevice(uid: Self.virtualUID) == virtualID else { return false }
            return CA.hasSettableVolume(currentDefault)
        case .driverMissing, .error:
            return false
        }
    }

    /// Nach Fehlern (z.B. TCC-Berechtigung noch nicht erteilt) periodisch
    /// erneut versuchen.
    private func scheduleRetry() {
        guard !retryScheduled else { return }
        retryScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 4.0) { [weak self] in
            guard let self else { return }
            self.retryScheduled = false
            if case .error = self.mode {
                self.rebuild()
            }
        }
    }
}
