import CoreAudio
import Foundation

/// Kleine Helfer, um die C-lastige CoreAudio-Property-API ertraeglich zu machen.
enum CA {
    static func address(
        _ selector: AudioObjectPropertySelector,
        scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
        element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain
    ) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: element)
    }

    static func get<T>(_ objectID: AudioObjectID, _ addr: AudioObjectPropertyAddress, defaultValue: T) -> T? {
        var addr = addr
        var value = defaultValue
        var size = UInt32(MemoryLayout<T>.size)
        let err = AudioObjectGetPropertyData(objectID, &addr, 0, nil, &size, &value)
        return err == noErr ? value : nil
    }

    @discardableResult
    static func set<T>(_ objectID: AudioObjectID, _ addr: AudioObjectPropertyAddress, value: T) -> Bool {
        var addr = addr
        var value = value
        let size = UInt32(MemoryLayout<T>.size)
        return AudioObjectSetPropertyData(objectID, &addr, 0, nil, size, &value) == noErr
    }

    static func getString(_ objectID: AudioObjectID, _ addr: AudioObjectPropertyAddress) -> String? {
        var addr = addr
        var value: CFString?
        var size = UInt32(MemoryLayout<CFString?>.size)
        let err = withUnsafeMutablePointer(to: &value) { ptr in
            AudioObjectGetPropertyData(objectID, &addr, 0, nil, &size, ptr)
        }
        guard err == noErr, let value else { return nil }
        return value as String
    }

    // MARK: - Geraete

    static func allDevices() -> [AudioObjectID] {
        var addr = address(kAudioHardwarePropertyDevices)
        var size: UInt32 = 0
        let system = AudioObjectID(kAudioObjectSystemObject)
        guard AudioObjectGetPropertyDataSize(system, &addr, 0, nil, &size) == noErr, size > 0 else { return [] }
        var devices = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(system, &addr, 0, nil, &size, &devices) == noErr else { return [] }
        return devices
    }

    static func hasOutputStreams(_ deviceID: AudioObjectID) -> Bool {
        var addr = address(kAudioDevicePropertyStreams, scope: kAudioDevicePropertyScopeOutput)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(deviceID, &addr, 0, nil, &size) == noErr else { return false }
        return size > 0
    }

    static func outputDevices() -> [AudioObjectID] {
        allDevices().filter { hasOutputStreams($0) }
    }

    static func findDevice(uid: String) -> AudioObjectID? {
        allDevices().first { deviceUID($0) == uid }
    }

    static func defaultOutputDevice() -> AudioObjectID? {
        let id = get(
            AudioObjectID(kAudioObjectSystemObject),
            address(kAudioHardwarePropertyDefaultOutputDevice),
            defaultValue: AudioObjectID(kAudioObjectUnknown)
        )
        guard let id, id != kAudioObjectUnknown else { return nil }
        return id
    }

    /// Setzt Standard-Ausgabegeraet UND das Geraet fuer Systemtoene.
    static func setDefaultOutput(_ deviceID: AudioObjectID) {
        let system = AudioObjectID(kAudioObjectSystemObject)
        set(system, address(kAudioHardwarePropertyDefaultOutputDevice), value: deviceID)
        set(system, address(kAudioHardwarePropertyDefaultSystemOutputDevice), value: deviceID)
    }

    static func deviceUID(_ deviceID: AudioObjectID) -> String? {
        getString(deviceID, address(kAudioDevicePropertyDeviceUID))
    }

    static func deviceName(_ deviceID: AudioObjectID) -> String {
        getString(deviceID, address(kAudioObjectPropertyName)) ?? "Unbekanntes Gerät"
    }

    static func nominalSampleRate(_ deviceID: AudioObjectID) -> Float64? {
        get(deviceID, address(kAudioDevicePropertyNominalSampleRate), defaultValue: Float64(0))
    }

    /// Hat das Geraet eine per Software setzbare Hardware-Lautstaerke?
    static func hasSettableVolume(_ deviceID: AudioObjectID) -> Bool {
        let elements: [AudioObjectPropertyElement] = [kAudioObjectPropertyElementMain, 1, 2]
        for element in elements {
            var addr = address(kAudioDevicePropertyVolumeScalar, scope: kAudioDevicePropertyScopeOutput, element: element)
            guard AudioObjectHasProperty(deviceID, &addr) else { continue }
            var settable: DarwinBoolean = false
            if AudioObjectIsPropertySettable(deviceID, &addr, &settable) == noErr, settable.boolValue {
                return true
            }
        }
        return false
    }

    /// AudioObjectID des HAL-Prozessobjekts fuer unsere eigene PID
    /// (noetig, um uns selbst vom Tap auszuschliessen — sonst Feedback-Schleife).
    static func ownProcessObject() -> AudioObjectID? {
        var pid = pid_t(ProcessInfo.processInfo.processIdentifier)
        var addr = address(kAudioHardwarePropertyTranslatePIDToProcessObject)
        var object = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let err = withUnsafeMutablePointer(to: &pid) { pidPtr in
            AudioObjectGetPropertyData(
                AudioObjectID(kAudioObjectSystemObject), &addr,
                UInt32(MemoryLayout<pid_t>.size), pidPtr, &size, &object
            )
        }
        guard err == noErr, object != kAudioObjectUnknown else { return nil }
        return object
    }
}
