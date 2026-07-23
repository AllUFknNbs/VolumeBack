/*
 * VolumeBack – minimaler AudioServerPlugIn-Treiber.
 *
 * Stellt genau ein virtuelles Ausgabegeraet ("VolumeBack") bereit, dessen
 * einziger Zweck ein nativer Lautstaerke- und Mute-Regler ist. Das Geraet
 * verwirft alles Audio (Null-Sink); die VolumeBack-App greift das Audio per
 * Process-Tap ab, liest den Reglerwert dieses Geraets und spielt das Signal
 * mit entsprechendem Gain auf dem echten Geraet (Monitor) aus.
 *
 * Aufbau nach Apples NullAudio-Beispiel, radikal eingedampft.
 * Objekt-IDs: 1 = PlugIn, 2 = Device, 3 = Output-Stream, 4 = Volume, 5 = Mute.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------- Konstanten

#define kPlugIn_BundleID        "dev.volumeback.driver"
#define kDevice_UID             "VolumeBack_Device_UID"
#define kDevice_ModelUID        "VolumeBack_Model_UID"
#define kDevice_Name            "VolumeBack"
#define kManufacturer_Name      "VolumeBack"

enum {
    kObjectID_PlugIn        = kAudioObjectPlugInObject, // 1
    kObjectID_Device        = 2,
    kObjectID_Stream_Output = 3,
    kObjectID_Volume_Output = 4,
    kObjectID_Mute_Output   = 5,
};

#define kDevice_RingFrames      16384u
#define kChannels               2u
#define kVolume_MinDB           (-64.0f)
#define kVolume_MaxDB           (0.0f)

static const Float64 kSupportedRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
#define kNumSupportedRates (sizeof(kSupportedRates) / sizeof(Float64))

// ------------------------------------------------------------------- Zustand

static pthread_mutex_t             gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef    gPlugIn_Host = NULL;
static UInt32                      gPlugIn_RefCount = 0;

static Float64                     gDevice_SampleRate = 48000.0;
static Float64                     gDevice_HostTicksPerFrame = 0.0;
static UInt64                      gDevice_IOIsRunning = 0;
static Float64                     gDevice_AnchorSampleTime = 0.0;
static UInt64                      gDevice_AnchorHostTime = 0;
static UInt64                      gDevice_NumberTimeStamps = 0;

static Float32                     gVolume_Scalar = 0.75f;
static Boolean                     gMute_Value = false;

static Boolean                     gStream_IsActive = true;

/* Anzeigename des Geraets — von der App setzbar, damit im nativen
 * Lautstaerkeregler "VolumeBack (<echtes Geraet>)" steht.
 *
 * Wichtig: coreaudiod blockiert Set-Aufrufe von Clients auf
 * kAudioObjectPropertyName ('nope'), bevor sie den Treiber erreichen.
 * Deshalb laeuft das Umbenennen ueber eine Custom Property ('vbnm'),
 * die coreaudiod ungefiltert durchreicht; der Treiber aendert daraufhin
 * seinen Namen selbst und meldet die Aenderung. */
static CFStringRef                 gDevice_Name_Value = NULL;

#define kCustomProperty_DeviceName 'vbnm'

// -------------------------------------------------------------------- Helfer

static Float32 ScalarToDB(Float32 scalar)
{
    if (scalar < 0.0f) scalar = 0.0f;
    if (scalar > 1.0f) scalar = 1.0f;
    return kVolume_MinDB + scalar * (kVolume_MaxDB - kVolume_MinDB);
}

static Float32 DBToScalar(Float32 db)
{
    if (db < kVolume_MinDB) db = kVolume_MinDB;
    if (db > kVolume_MaxDB) db = kVolume_MaxDB;
    return (db - kVolume_MinDB) / (kVolume_MaxDB - kVolume_MinDB);
}

static void UpdateHostTicksPerFrame(void)
{
    struct mach_timebase_info info;
    mach_timebase_info(&info);
    Float64 hostClockFrequency = ((Float64)info.denom / (Float64)info.numer) * 1000000000.0;
    gDevice_HostTicksPerFrame = hostClockFrequency / gDevice_SampleRate;
}

static void FillASBD(AudioStreamBasicDescription* asbd, Float64 rate)
{
    memset(asbd, 0, sizeof(*asbd));
    asbd->mSampleRate       = rate;
    asbd->mFormatID         = kAudioFormatLinearPCM;
    asbd->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    asbd->mBytesPerPacket   = kChannels * sizeof(Float32);
    asbd->mFramesPerPacket  = 1;
    asbd->mBytesPerFrame    = kChannels * sizeof(Float32);
    asbd->mChannelsPerFrame = kChannels;
    asbd->mBitsPerChannel   = 32;
}

static Boolean RateIsSupported(Float64 rate)
{
    for (size_t i = 0; i < kNumSupportedRates; i++) {
        if (kSupportedRates[i] == rate) return true;
    }
    return false;
}

// ------------------------------------------------------- Interface-Vorwaertsdeklaration

static AudioServerPlugInDriverInterface gInterface;
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

// --------------------------------------------------------------- COM-Geruest

static HRESULT VB_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (inDriver != gDriverRef || outInterface == NULL) return E_POINTER;

    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (requested == NULL) return E_POINTER;

    CFUUIDRef iunknown = CFUUIDGetConstantUUIDWithBytes(NULL, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
    Boolean matches = CFEqual(requested, iunknown) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(requested);

    if (!matches) {
        *outInterface = NULL;
        return E_NOINTERFACE;
    }

    pthread_mutex_lock(&gStateMutex);
    gPlugIn_RefCount++;
    pthread_mutex_unlock(&gStateMutex);
    *outInterface = gDriverRef;
    return S_OK;
}

static ULONG VB_AddRef(void* inDriver)
{
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    ULONG count = ++gPlugIn_RefCount;
    pthread_mutex_unlock(&gStateMutex);
    return count;
}

static ULONG VB_Release(void* inDriver)
{
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gPlugIn_RefCount > 0) gPlugIn_RefCount--;
    ULONG count = gPlugIn_RefCount;
    pthread_mutex_unlock(&gStateMutex);
    return count;
}

// ----------------------------------------------------------- Basis-Callbacks

static OSStatus VB_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gPlugIn_Host = inHost;
    if (gDevice_Name_Value == NULL) {
        gDevice_Name_Value = CFStringCreateCopy(NULL, CFSTR(kDevice_Name));
    }
    UpdateHostTicksPerFrame();
    return kAudioHardwareNoError;
}

static OSStatus VB_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
                                const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID)
{
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus VB_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID)
{
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus VB_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                   const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return kAudioHardwareNoError;
}

static OSStatus VB_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                      const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return kAudioHardwareNoError;
}

static OSStatus VB_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                    UInt64 inChangeAction, void* inChangeInfo)
{
    (void)inChangeInfo;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (!RateIsSupported((Float64)inChangeAction)) return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gStateMutex);
    gDevice_SampleRate = (Float64)inChangeAction;
    UpdateHostTicksPerFrame();
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus VB_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                  UInt64 inChangeAction, void* inChangeInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return kAudioHardwareNoError;
}

// ---------------------------------------------------------------- Properties

static Boolean VB_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientPID,
                              const AudioObjectPropertyAddress* inAddress)
{
    (void)inClientPID;
    if (inDriver != gDriverRef || inAddress == NULL) return false;

    switch (inObjectID) {
        case kObjectID_PlugIn:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                case kAudioPlugInPropertyTranslateUIDToDevice:
                case kAudioPlugInPropertyResourceBundle:
                    return true;
            }
            return false;

        case kObjectID_Device:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioObjectPropertyControlList:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyRelatedDevices:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertyStreams:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyAvailableNominalSampleRates:
                case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                case kAudioDevicePropertyPreferredChannelsForStereo:
                case kAudioDevicePropertyPreferredChannelLayout:
                case kAudioObjectPropertyCustomPropertyInfoList:
                case kCustomProperty_DeviceName:
                    return true;
            }
            return false;

        case kObjectID_Stream_Output:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency:
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    return true;
            }
            return false;

        case kObjectID_Volume_Output:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                case kAudioLevelControlPropertyScalarValue:
                case kAudioLevelControlPropertyDecibelValue:
                case kAudioLevelControlPropertyDecibelRange:
                case kAudioLevelControlPropertyConvertScalarToDecibels:
                case kAudioLevelControlPropertyConvertDecibelsToScalar:
                    return true;
            }
            return false;

        case kObjectID_Mute_Output:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                case kAudioBooleanControlPropertyValue:
                    return true;
            }
            return false;
    }
    return false;
}

static OSStatus VB_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientPID,
                                      const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
    (void)inClientPID;
    if (inDriver != gDriverRef || inAddress == NULL || outIsSettable == NULL) return kAudioHardwareBadObjectError;
    if (!VB_HasProperty(inDriver, inObjectID, inClientPID, inAddress)) return kAudioHardwareUnknownPropertyError;

    *outIsSettable = false;
    switch (inObjectID) {
        case kObjectID_Device:
            if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate ||
                inAddress->mSelector == kCustomProperty_DeviceName) *outIsSettable = true;
            break;
        case kObjectID_Stream_Output:
            if (inAddress->mSelector == kAudioStreamPropertyIsActive ||
                inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
                inAddress->mSelector == kAudioStreamPropertyPhysicalFormat) *outIsSettable = true;
            break;
        case kObjectID_Volume_Output:
            if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue ||
                inAddress->mSelector == kAudioLevelControlPropertyDecibelValue) *outIsSettable = true;
            break;
        case kObjectID_Mute_Output:
            if (inAddress->mSelector == kAudioBooleanControlPropertyValue) *outIsSettable = true;
            break;
    }
    return kAudioHardwareNoError;
}

static OSStatus VB_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientPID,
                                       const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize,
                                       const void* inQualifierData, UInt32* outDataSize)
{
    (void)inClientPID; (void)inQualifierDataSize; (void)inQualifierData;
    if (inDriver != gDriverRef || inAddress == NULL || outDataSize == NULL) return kAudioHardwareBadObjectError;

    switch (inObjectID) {
        case kObjectID_PlugIn:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyManufacturer:
                case kAudioPlugInPropertyResourceBundle:
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioPlugInPropertyTranslateUIDToDevice:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            }
            break;

        case kObjectID_Device:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects:
                    *outDataSize = 3 * sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioObjectPropertyControlList:
                    *outDataSize = 2 * sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyRelatedDevices:
                case kAudioDevicePropertyStreams:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioDevicePropertyNominalSampleRate:
                    *outDataSize = sizeof(Float64); return kAudioHardwareNoError;
                case kAudioDevicePropertyAvailableNominalSampleRates:
                    *outDataSize = kNumSupportedRates * sizeof(AudioValueRange); return kAudioHardwareNoError;
                case kAudioDevicePropertyPreferredChannelsForStereo:
                    *outDataSize = 2 * sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyPreferredChannelLayout:
                    *outDataSize = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions)
                                            + kChannels * sizeof(AudioChannelDescription));
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyCustomPropertyInfoList:
                    *outDataSize = sizeof(AudioServerPlugInCustomPropertyInfo); return kAudioHardwareNoError;
                case kCustomProperty_DeviceName:
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            }
            break;

        case kObjectID_Stream_Output:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency:
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                    *outDataSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    *outDataSize = kNumSupportedRates * sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
            }
            break;

        case kObjectID_Volume_Output:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioControlPropertyScope:
                    *outDataSize = sizeof(AudioObjectPropertyScope); return kAudioHardwareNoError;
                case kAudioControlPropertyElement:
                    *outDataSize = sizeof(AudioObjectPropertyElement); return kAudioHardwareNoError;
                case kAudioLevelControlPropertyScalarValue:
                case kAudioLevelControlPropertyDecibelValue:
                case kAudioLevelControlPropertyConvertScalarToDecibels:
                case kAudioLevelControlPropertyConvertDecibelsToScalar:
                    *outDataSize = sizeof(Float32); return kAudioHardwareNoError;
                case kAudioLevelControlPropertyDecibelRange:
                    *outDataSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
            }
            break;

        case kObjectID_Mute_Output:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioControlPropertyScope:
                    *outDataSize = sizeof(AudioObjectPropertyScope); return kAudioHardwareNoError;
                case kAudioControlPropertyElement:
                    *outDataSize = sizeof(AudioObjectPropertyElement); return kAudioHardwareNoError;
                case kAudioBooleanControlPropertyValue:
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            }
            break;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus VB_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientPID,
                                   const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize,
                                   const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    (void)inClientPID;
    if (inDriver != gDriverRef || inAddress == NULL || outDataSize == NULL || outData == NULL)
        return kAudioHardwareBadObjectError;

    switch (inObjectID) {
        // ------------------------------------------------------------ PlugIn
        case kObjectID_PlugIn:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioClassID*)outData) = kAudioObjectClassID;
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioClassID*)outData) = kAudioPlugInClassID;
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioObjectID*)outData) = kAudioObjectUnknown;
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioObjectPropertyManufacturer:
                    if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                    *((CFStringRef*)outData) = CFSTR(kManufacturer_Name);
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                    if (inDataSize < sizeof(AudioObjectID)) { *outDataSize = 0; return kAudioHardwareNoError; }
                    *((AudioObjectID*)outData) = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioPlugInPropertyTranslateUIDToDevice: {
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    if (inQualifierDataSize != sizeof(CFStringRef) || inQualifierData == NULL)
                        return kAudioHardwareBadPropertySizeError;
                    CFStringRef uid = *((CFStringRef*)inQualifierData);
                    if (uid != NULL && CFStringCompare(uid, CFSTR(kDevice_UID), 0) == kCFCompareEqualTo)
                        *((AudioObjectID*)outData) = kObjectID_Device;
                    else
                        *((AudioObjectID*)outData) = kAudioObjectUnknown;
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                }
                case kAudioPlugInPropertyResourceBundle:
                    if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                    *((CFStringRef*)outData) = CFSTR("");
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            }
            break;

        // ------------------------------------------------------------ Device
        case kObjectID_Device:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioClassID*)outData) = kAudioObjectClassID;
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioClassID*)outData) = kAudioDeviceClassID;
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioObjectID*)outData) = kObjectID_PlugIn;
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioObjectPropertyName:
                    if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                    pthread_mutex_lock(&gStateMutex);
                    *((CFStringRef*)outData) = CFStringCreateCopy(
                        NULL, gDevice_Name_Value != NULL ? gDevice_Name_Value : CFSTR(kDevice_Name));
                    pthread_mutex_unlock(&gStateMutex);
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioObjectPropertyManufacturer:
                    if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                    *((CFStringRef*)outData) = CFSTR(kManufacturer_Name);
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects: {
                    UInt32 count = inDataSize / sizeof(AudioObjectID);
                    if (count > 3) count = 3;
                    AudioObjectID objects[3] = { kObjectID_Stream_Output, kObjectID_Volume_Output, kObjectID_Mute_Output };
                    memcpy(outData, objects, count * sizeof(AudioObjectID));
                    *outDataSize = count * sizeof(AudioObjectID); return kAudioHardwareNoError;
                }
                case kAudioObjectPropertyControlList: {
                    UInt32 count = inDataSize / sizeof(AudioObjectID);
                    if (count > 2) count = 2;
                    AudioObjectID controls[2] = { kObjectID_Volume_Output, kObjectID_Mute_Output };
                    memcpy(outData, controls, count * sizeof(AudioObjectID));
                    *outDataSize = count * sizeof(AudioObjectID); return kAudioHardwareNoError;
                }
                case kAudioDevicePropertyDeviceUID:
                    if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                    *((CFStringRef*)outData) = CFSTR(kDevice_UID);
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioDevicePropertyModelUID:
                    if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                    *((CFStringRef*)outData) = CFSTR(kDevice_ModelUID);
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioDevicePropertyTransportType:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = kAudioDeviceTransportTypeVirtual;
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyRelatedDevices:
                    if (inDataSize < sizeof(AudioObjectID)) { *outDataSize = 0; return kAudioHardwareNoError; }
                    *((AudioObjectID*)outData) = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioDevicePropertyClockDomain:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = 0;
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyDeviceIsAlive:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = 1;
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyDeviceIsRunning:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    pthread_mutex_lock(&gStateMutex);
                    *((UInt32*)outData) = (gDevice_IOIsRunning > 0) ? 1 : 0;
                    pthread_mutex_unlock(&gStateMutex);
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = 1;
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = 0;
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyStreams:
                    if ((inAddress->mScope == kAudioObjectPropertyScopeGlobal ||
                         inAddress->mScope == kAudioObjectPropertyScopeOutput) &&
                        inDataSize >= sizeof(AudioObjectID)) {
                        *((AudioObjectID*)outData) = kObjectID_Stream_Output;
                        *outDataSize = sizeof(AudioObjectID);
                    } else {
                        *outDataSize = 0;
                    }
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyNominalSampleRate:
                    if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
                    pthread_mutex_lock(&gStateMutex);
                    *((Float64*)outData) = gDevice_SampleRate;
                    pthread_mutex_unlock(&gStateMutex);
                    *outDataSize = sizeof(Float64); return kAudioHardwareNoError;
                case kAudioDevicePropertyAvailableNominalSampleRates: {
                    UInt32 count = inDataSize / sizeof(AudioValueRange);
                    if (count > kNumSupportedRates) count = kNumSupportedRates;
                    AudioValueRange* ranges = (AudioValueRange*)outData;
                    for (UInt32 i = 0; i < count; i++) {
                        ranges[i].mMinimum = kSupportedRates[i];
                        ranges[i].mMaximum = kSupportedRates[i];
                    }
                    *outDataSize = count * sizeof(AudioValueRange); return kAudioHardwareNoError;
                }
                case kAudioDevicePropertyIsHidden:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = 0;
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyZeroTimeStampPeriod:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = kDevice_RingFrames;
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyPreferredChannelsForStereo:
                    if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    ((UInt32*)outData)[0] = 1;
                    ((UInt32*)outData)[1] = 2;
                    *outDataSize = 2 * sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioObjectPropertyCustomPropertyInfoList:
                    if (inDataSize < sizeof(AudioServerPlugInCustomPropertyInfo)) {
                        *outDataSize = 0; return kAudioHardwareNoError;
                    }
                    ((AudioServerPlugInCustomPropertyInfo*)outData)->mSelector = kCustomProperty_DeviceName;
                    ((AudioServerPlugInCustomPropertyInfo*)outData)->mPropertyDataType =
                        kAudioServerPlugInCustomPropertyDataTypeCFString;
                    ((AudioServerPlugInCustomPropertyInfo*)outData)->mQualifierDataType =
                        kAudioServerPlugInCustomPropertyDataTypeNone;
                    *outDataSize = sizeof(AudioServerPlugInCustomPropertyInfo); return kAudioHardwareNoError;
                case kCustomProperty_DeviceName:
                    if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                    pthread_mutex_lock(&gStateMutex);
                    *((CFStringRef*)outData) = CFStringCreateCopy(
                        NULL, gDevice_Name_Value != NULL ? gDevice_Name_Value : CFSTR(kDevice_Name));
                    pthread_mutex_unlock(&gStateMutex);
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioDevicePropertyPreferredChannelLayout: {
                    UInt32 needed = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions)
                                             + kChannels * sizeof(AudioChannelDescription));
                    if (inDataSize < needed) return kAudioHardwareBadPropertySizeError;
                    AudioChannelLayout* layout = (AudioChannelLayout*)outData;
                    layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                    layout->mChannelBitmap = 0;
                    layout->mNumberChannelDescriptions = kChannels;
                    for (UInt32 i = 0; i < kChannels; i++) {
                        layout->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Left + i;
                        layout->mChannelDescriptions[i].mChannelFlags = 0;
                        memset(layout->mChannelDescriptions[i].mCoordinates, 0,
                               sizeof(layout->mChannelDescriptions[i].mCoordinates));
                    }
                    *outDataSize = needed; return kAudioHardwareNoError;
                }
            }
            break;

        // ------------------------------------------------------------ Stream
        case kObjectID_Stream_Output:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioClassID*)outData) = kAudioObjectClassID;
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioClassID*)outData) = kAudioStreamClassID;
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioObjectID*)outData) = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioStreamPropertyIsActive:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    pthread_mutex_lock(&gStateMutex);
                    *((UInt32*)outData) = gStream_IsActive ? 1 : 0;
                    pthread_mutex_unlock(&gStateMutex);
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioStreamPropertyDirection:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = 0; // Output
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioStreamPropertyTerminalType:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = kAudioStreamTerminalTypeSpeaker;
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioStreamPropertyStartingChannel:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = 1;
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioStreamPropertyLatency:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *((UInt32*)outData) = 0;
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                    if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                    pthread_mutex_lock(&gStateMutex);
                    FillASBD((AudioStreamBasicDescription*)outData, gDevice_SampleRate);
                    pthread_mutex_unlock(&gStateMutex);
                    *outDataSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats: {
                    UInt32 count = inDataSize / sizeof(AudioStreamRangedDescription);
                    if (count > kNumSupportedRates) count = kNumSupportedRates;
                    AudioStreamRangedDescription* formats = (AudioStreamRangedDescription*)outData;
                    for (UInt32 i = 0; i < count; i++) {
                        FillASBD(&formats[i].mFormat, kSupportedRates[i]);
                        formats[i].mSampleRateRange.mMinimum = kSupportedRates[i];
                        formats[i].mSampleRateRange.mMaximum = kSupportedRates[i];
                    }
                    *outDataSize = count * sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
                }
            }
            break;

        // ---------------------------------------------------- Volume-Control
        case kObjectID_Volume_Output:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioClassID*)outData) = kAudioLevelControlClassID;
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioClassID*)outData) = kAudioVolumeControlClassID;
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioObjectID*)outData) = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioControlPropertyScope:
                    if (inDataSize < sizeof(AudioObjectPropertyScope)) return kAudioHardwareBadPropertySizeError;
                    *((AudioObjectPropertyScope*)outData) = kAudioObjectPropertyScopeOutput;
                    *outDataSize = sizeof(AudioObjectPropertyScope); return kAudioHardwareNoError;
                case kAudioControlPropertyElement:
                    if (inDataSize < sizeof(AudioObjectPropertyElement)) return kAudioHardwareBadPropertySizeError;
                    *((AudioObjectPropertyElement*)outData) = kAudioObjectPropertyElementMain;
                    *outDataSize = sizeof(AudioObjectPropertyElement); return kAudioHardwareNoError;
                case kAudioLevelControlPropertyScalarValue:
                    if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                    pthread_mutex_lock(&gStateMutex);
                    *((Float32*)outData) = gVolume_Scalar;
                    pthread_mutex_unlock(&gStateMutex);
                    *outDataSize = sizeof(Float32); return kAudioHardwareNoError;
                case kAudioLevelControlPropertyDecibelValue:
                    if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                    pthread_mutex_lock(&gStateMutex);
                    *((Float32*)outData) = ScalarToDB(gVolume_Scalar);
                    pthread_mutex_unlock(&gStateMutex);
                    *outDataSize = sizeof(Float32); return kAudioHardwareNoError;
                case kAudioLevelControlPropertyDecibelRange:
                    if (inDataSize < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
                    ((AudioValueRange*)outData)->mMinimum = kVolume_MinDB;
                    ((AudioValueRange*)outData)->mMaximum = kVolume_MaxDB;
                    *outDataSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
                case kAudioLevelControlPropertyConvertScalarToDecibels:
                    if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                    *((Float32*)outData) = ScalarToDB(*((Float32*)outData));
                    *outDataSize = sizeof(Float32); return kAudioHardwareNoError;
                case kAudioLevelControlPropertyConvertDecibelsToScalar:
                    if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                    *((Float32*)outData) = DBToScalar(*((Float32*)outData));
                    *outDataSize = sizeof(Float32); return kAudioHardwareNoError;
            }
            break;

        // ------------------------------------------------------ Mute-Control
        case kObjectID_Mute_Output:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioClassID*)outData) = kAudioBooleanControlClassID;
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioClassID*)outData) = kAudioMuteControlClassID;
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *((AudioObjectID*)outData) = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioControlPropertyScope:
                    if (inDataSize < sizeof(AudioObjectPropertyScope)) return kAudioHardwareBadPropertySizeError;
                    *((AudioObjectPropertyScope*)outData) = kAudioObjectPropertyScopeOutput;
                    *outDataSize = sizeof(AudioObjectPropertyScope); return kAudioHardwareNoError;
                case kAudioControlPropertyElement:
                    if (inDataSize < sizeof(AudioObjectPropertyElement)) return kAudioHardwareBadPropertySizeError;
                    *((AudioObjectPropertyElement*)outData) = kAudioObjectPropertyElementMain;
                    *outDataSize = sizeof(AudioObjectPropertyElement); return kAudioHardwareNoError;
                case kAudioBooleanControlPropertyValue:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    pthread_mutex_lock(&gStateMutex);
                    *((UInt32*)outData) = gMute_Value ? 1 : 0;
                    pthread_mutex_unlock(&gStateMutex);
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            }
            break;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus VB_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientPID,
                                   const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize,
                                   const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
    (void)inClientPID; (void)inQualifierDataSize; (void)inQualifierData;
    if (inDriver != gDriverRef || inAddress == NULL || inData == NULL) return kAudioHardwareBadObjectError;

    switch (inObjectID) {
        case kObjectID_Device:
            if (inAddress->mSelector == kCustomProperty_DeviceName) {
                if (inDataSize != sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                CFStringRef newName = *((CFStringRef*)inData);
                if (newName == NULL || CFGetTypeID(newName) != CFStringGetTypeID())
                    return kAudioHardwareIllegalOperationError;
                CFStringRef copy = CFStringCreateCopy(NULL, newName);
                if (copy == NULL) return kAudioHardwareIllegalOperationError;

                pthread_mutex_lock(&gStateMutex);
                CFStringRef old = gDevice_Name_Value;
                gDevice_Name_Value = copy;
                pthread_mutex_unlock(&gStateMutex);
                if (old != NULL) CFRelease(old);

                if (gPlugIn_Host != NULL) {
                    AudioObjectPropertyAddress changes[2] = {
                        { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                        { kCustomProperty_DeviceName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                    };
                    gPlugIn_Host->PropertiesChanged(gPlugIn_Host, kObjectID_Device, 2, changes);
                }
                return kAudioHardwareNoError;
            }
            if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
                if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
                Float64 newRate = *((const Float64*)inData);
                if (!RateIsSupported(newRate)) return kAudioDeviceUnsupportedFormatError;
                pthread_mutex_lock(&gStateMutex);
                Boolean different = (newRate != gDevice_SampleRate);
                pthread_mutex_unlock(&gStateMutex);
                if (different && gPlugIn_Host != NULL) {
                    gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device,
                                                                   (UInt64)newRate, NULL);
                }
                return kAudioHardwareNoError;
            }
            break;

        case kObjectID_Stream_Output:
            if (inAddress->mSelector == kAudioStreamPropertyIsActive) {
                if (inDataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                gStream_IsActive = (*((const UInt32*)inData) != 0);
                pthread_mutex_unlock(&gStateMutex);
                return kAudioHardwareNoError;
            }
            if (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
                inAddress->mSelector == kAudioStreamPropertyPhysicalFormat) {
                if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                const AudioStreamBasicDescription* asbd = (const AudioStreamBasicDescription*)inData;
                if (asbd->mFormatID != kAudioFormatLinearPCM || asbd->mChannelsPerFrame != kChannels ||
                    asbd->mBitsPerChannel != 32 || !RateIsSupported(asbd->mSampleRate))
                    return kAudioDeviceUnsupportedFormatError;
                pthread_mutex_lock(&gStateMutex);
                Boolean different = (asbd->mSampleRate != gDevice_SampleRate);
                pthread_mutex_unlock(&gStateMutex);
                if (different && gPlugIn_Host != NULL) {
                    gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device,
                                                                   (UInt64)asbd->mSampleRate, NULL);
                }
                return kAudioHardwareNoError;
            }
            break;

        case kObjectID_Volume_Output: {
            Float32 newScalar;
            if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue) {
                if (inDataSize != sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                newScalar = *((const Float32*)inData);
            } else if (inAddress->mSelector == kAudioLevelControlPropertyDecibelValue) {
                if (inDataSize != sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                newScalar = DBToScalar(*((const Float32*)inData));
            } else {
                break;
            }
            if (newScalar < 0.0f) newScalar = 0.0f;
            if (newScalar > 1.0f) newScalar = 1.0f;

            pthread_mutex_lock(&gStateMutex);
            Boolean changed = (newScalar != gVolume_Scalar);
            gVolume_Scalar = newScalar;
            pthread_mutex_unlock(&gStateMutex);

            if (changed && gPlugIn_Host != NULL) {
                AudioObjectPropertyAddress changes[2] = {
                    { kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                    { kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                };
                gPlugIn_Host->PropertiesChanged(gPlugIn_Host, kObjectID_Volume_Output, 2, changes);
            }
            return kAudioHardwareNoError;
        }

        case kObjectID_Mute_Output:
            if (inAddress->mSelector == kAudioBooleanControlPropertyValue) {
                if (inDataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                Boolean newValue = (*((const UInt32*)inData) != 0);

                pthread_mutex_lock(&gStateMutex);
                Boolean changed = (newValue != gMute_Value);
                gMute_Value = newValue;
                pthread_mutex_unlock(&gStateMutex);

                if (changed && gPlugIn_Host != NULL) {
                    AudioObjectPropertyAddress change = {
                        kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
                    };
                    gPlugIn_Host->PropertiesChanged(gPlugIn_Host, kObjectID_Mute_Output, 1, &change);
                }
                return kAudioHardwareNoError;
            }
            break;
    }
    return kAudioHardwareUnknownPropertyError;
}

// ------------------------------------------------------------------------ IO

static OSStatus VB_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gStateMutex);
    if (gDevice_IOIsRunning == UINT64_MAX) {
        pthread_mutex_unlock(&gStateMutex);
        return kAudioHardwareIllegalOperationError;
    }
    if (gDevice_IOIsRunning == 0) {
        gDevice_NumberTimeStamps = 0;
        gDevice_AnchorSampleTime = 0;
        gDevice_AnchorHostTime = mach_absolute_time();
    }
    gDevice_IOIsRunning++;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus VB_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gStateMutex);
    if (gDevice_IOIsRunning > 0) gDevice_IOIsRunning--;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus VB_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                    UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (outSampleTime == NULL || outHostTime == NULL || outSeed == NULL) return kAudioHardwareIllegalOperationError;

    pthread_mutex_lock(&gStateMutex);
    UInt64 currentHostTime = mach_absolute_time();
    Float64 hostTicksPerPeriod = gDevice_HostTicksPerFrame * (Float64)kDevice_RingFrames;
    UInt64 nextHostTime = gDevice_AnchorHostTime
        + (UInt64)(((Float64)(gDevice_NumberTimeStamps + 1)) * hostTicksPerPeriod);
    if (nextHostTime <= currentHostTime) {
        gDevice_NumberTimeStamps++;
    }
    *outSampleTime = (Float64)(gDevice_NumberTimeStamps * kDevice_RingFrames);
    *outHostTime = gDevice_AnchorHostTime + (UInt64)((Float64)gDevice_NumberTimeStamps * hostTicksPerPeriod);
    *outSeed = 1;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus VB_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                     UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo,
                                     Boolean* outWillDoInPlace)
{
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (outWillDo == NULL || outWillDoInPlace == NULL) return kAudioHardwareIllegalOperationError;

    *outWillDo = (inOperationID == kAudioServerPlugInIOOperationWriteMix);
    *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus VB_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                    UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                    const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID; (void)inOperationID;
    (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return kAudioHardwareNoError;
}

static OSStatus VB_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                 AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID,
                                 UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo,
                                 void* ioMainBuffer, void* ioSecondaryBuffer)
{
    (void)inStreamObjectID; (void)inClientID; (void)inIOCycleInfo; (void)ioSecondaryBuffer;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    // Null-Sink: geschriebenes Audio wird verworfen. Der Puffer wird genullt,
    // damit keine alten Daten in Folgezyklen auftauchen.
    if (inOperationID == kAudioServerPlugInIOOperationWriteMix && ioMainBuffer != NULL) {
        memset(ioMainBuffer, 0, inIOBufferFrameSize * kChannels * sizeof(Float32));
    }
    return kAudioHardwareNoError;
}

static OSStatus VB_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                  UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                  const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID; (void)inOperationID;
    (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return kAudioHardwareNoError;
}

// ----------------------------------------------------------------- Interface

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    VB_QueryInterface,
    VB_AddRef,
    VB_Release,
    VB_Initialize,
    VB_CreateDevice,
    VB_DestroyDevice,
    VB_AddDeviceClient,
    VB_RemoveDeviceClient,
    VB_PerformDeviceConfigurationChange,
    VB_AbortDeviceConfigurationChange,
    VB_HasProperty,
    VB_IsPropertySettable,
    VB_GetPropertyDataSize,
    VB_GetPropertyData,
    VB_SetPropertyData,
    VB_StartIO,
    VB_StopIO,
    VB_GetZeroTimeStamp,
    VB_WillDoIOOperation,
    VB_BeginIOOperation,
    VB_DoIOOperation,
    VB_EndIOOperation,
};

// Factory — in Info.plist unter CFPlugInFactories referenziert.
void* VolumeBack_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);
void* VolumeBack_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    (void)inAllocator;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        return gDriverRef;
    }
    return NULL;
}
