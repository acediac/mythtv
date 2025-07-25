/*****************************************************************************
 * = NAME
 * audiooutputca.cpp
 *
 * = DESCRIPTION
 * Core Audio glue for Mac OS X.
 *
 * = REVISION
 * $Id$
 *
 * = AUTHORS
 * Jeremiah Morris, Andrew Kimpton, Nigel Pearson, Jean-Yves Avenard
 *****************************************************************************/

#include <SoundTouch.h>

#include <array>
#include <vector>

#include <CoreServices/CoreServices.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioFormat.h>
#include <AvailabilityMacros.h>

#include "libmythbase/mythcorecontext.h"
#include "libmythbase/mythlogging.h"
#include "audiooutputca.h"

// kAudioObjectPropertyElementMaster was deprecated in OS_X 12
// kAudioObjectPropertyElementMain defaults to a main/master port value of 0
static constexpr int8_t kMythAudioObjectPropertyElementMain { 0 };

#define LOC QString("CoreAudio: ")

#define CHANNELS_MIN 1
#define CHANNELS_MAX 8

using AudioStreamIDVec = std::vector<AudioStreamID>;
using AudioStreamRangedVec = std::vector<AudioStreamRangedDescription>;
using AudioValueRangeVec = std::vector<AudioValueRange>;
using RatesVec = std::vector<int>;
using ChannelsArr = std::array<bool,CHANNELS_MAX>;

#define OSS_STATUS(x) UInt32ToFourCC((UInt32*)&(x))
char* UInt32ToFourCC(const UInt32* pVal)
{
    UInt32 inVal = *pVal;
    char* pIn = (char*)&inVal;
    static char fourCC[5];
    fourCC[4] = 0;
    fourCC[3] = pIn[0];
    fourCC[2] = pIn[1];
    fourCC[1] = pIn[2];
    fourCC[0] = pIn[3];
    return fourCC;
}

QString StreamDescriptionToString(AudioStreamBasicDescription desc)
{
    UInt32 formatId = desc.mFormatID;
    char* fourCC = UInt32ToFourCC(&formatId);
    QString str;

    switch (desc.mFormatID)
    {
        case kAudioFormatLinearPCM:
            str = QString("[%1] %2%3 Channel %4-bit %5 %6 (%7Hz) %8 Channels")
            .arg(fourCC)
            .arg((desc.mFormatFlags & kAudioFormatFlagIsNonMixable) ? "" : "Mixable ")
            .arg(desc.mChannelsPerFrame)
            .arg(desc.mBitsPerChannel)
            .arg((desc.mFormatFlags & kAudioFormatFlagIsFloat) ? "Floating Point" : "Signed Integer")
            .arg((desc.mFormatFlags & kAudioFormatFlagIsBigEndian) ? "BE" : "LE")
            .arg((UInt32)desc.mSampleRate)
            .arg(desc.mChannelsPerFrame);
            break;
        case kAudioFormatAC3:
            str = QString("[%1] AC-3/DTS (%2Hz) %3 Channels")
            .arg(fourCC)
            .arg((UInt32)desc.mSampleRate)
            .arg(desc.mChannelsPerFrame);
            break;
        case kAudioFormat60958AC3:
            str = QString("[%1] AC-3/DTS for S/PDIF %2 (%3Hz) %4 Channels")
            .arg(fourCC)
            .arg((desc.mFormatFlags & kAudioFormatFlagIsBigEndian) ? "BE" : "LE")
            .arg((UInt32)desc.mSampleRate)
            .arg(desc.mChannelsPerFrame);
            break;
        default:
            str = QString("[%1]").arg(fourCC);
            break;
    }
    return str;
}

/** \class CoreAudioData
 *  \brief This holds Core Audio member variables and low-level audio IO methods
 * The name is now a misnomer, it should be CoreAudioPrivate, or CoreAudioMgr
 */
class CoreAudioData {
public:
    explicit CoreAudioData(AudioOutputCA *parent);
    CoreAudioData(AudioOutputCA *parent, AudioDeviceID deviceID);
    CoreAudioData(AudioOutputCA *parent, QString deviceName);

    static AudioDeviceID GetDefaultOutputDevice();
    int  GetTotalOutputChannels();
    QString *GetName();
    static AudioDeviceID GetDeviceWithName(const QString& deviceName);

    bool OpenDevice();
    int  OpenAnalog();
    void CloseAnalog();
    bool OpenSPDIF ();
    void CloseSPDIF();

    static void SetAutoHogMode(bool enable);
    static bool GetAutoHogMode();
    static pid_t GetHogStatus();
    bool SetHogStatus(bool hog);
    bool SetMixingSupport(bool mix);
    bool GetMixingSupport();

    bool FindAC3Stream();
    static void ResetAudioDevices();
    static void ResetStream(AudioStreamID s);
    static RatesVec RatesList(AudioDeviceID d);
    bool ChannelsList(AudioDeviceID d, bool passthru, ChannelsArr& chans);

    static AudioStreamIDVec      StreamsList(AudioDeviceID d);
    static AudioStreamRangedVec  FormatsList(AudioStreamID s);

    static int  AudioStreamChangeFormat(AudioStreamID s,
                                 AudioStreamBasicDescription format);

    AudioOutputCA  *mCA            {nullptr}; // We could subclass, but this ends up tidier

    // Analog output specific
    AudioUnit      mOutputUnit     {nullptr};

    // SPDIF mode specific
    bool           mDigitalInUse   {false};   // Is the digital (SPDIF) output in use?
    pid_t          mHog            {-1};
    int            mMixerRestore   {-1};
    AudioDeviceID  mDeviceID       {0};
    AudioStreamID  mStreamID       {};      // StreamID that has a cac3 streamformat
    int            mStreamIndex    {-1};    // Index of mStreamID in an AudioBufferList
    UInt32         mBytesPerPacket {UINT32_MAX};
    AudioStreamBasicDescription mFormatOrig {}; // The original format the stream
    AudioStreamBasicDescription mFormatNew  {}; // The format we changed the stream to
    bool           mRevertFormat  {false};  // Do we need to revert the stream format?
    bool           mIoProc        {false};
    bool           mInitialized   {false};
    bool           mStarted       {false};
    bool           mWasDigital    {false};
    AudioDeviceIOProcID mIoProcID {};
};

// These callbacks communicate with Core Audio.
static OSStatus RenderCallbackAnalog(void                       *inRefCon,
                                     AudioUnitRenderActionFlags *ioActionFlags,
                                     const AudioTimeStamp       *inTimeStamp,
                                     UInt32                     inBusNumber,
                                     UInt32                     inNumberFrames,
                                     AudioBufferList            *ioData);
static OSStatus RenderCallbackSPDIF(AudioDeviceID        inDevice,
                                    const AudioTimeStamp *inNow,
                                    const void           *inInputData,
                                    const AudioTimeStamp *inInputTime,
                                    AudioBufferList      *outOutputData,
                                    const AudioTimeStamp *inOutputTime,
                                    void                 *inRefCon);

/** \class AudioOutputCA
 *  \brief Implements Core Audio (Mac OS X Hardware Abstraction Layer) output.
 */

AudioOutputCA::AudioOutputCA(const AudioSettings &settings) :
AudioOutputBase(settings)
{
    m_mainDevice.remove(0, 10);
    LOG(VB_AUDIO, LOG_INFO, LOC + QString("AudioOutputCA::AudioOutputCA searching %1").arg(m_mainDevice));
    d = new CoreAudioData(this, m_mainDevice);

    InitSettings(settings);
    if (settings.m_init)
        Reconfigure(settings);
}

AudioOutputCA::~AudioOutputCA()
{
    KillAudio();

    delete d;
}

AudioOutputSettings* AudioOutputCA::GetOutputSettings(bool digital)
{
    auto *settings = new AudioOutputSettings();

    // Seek hardware sample rate available
    RatesVec rates = CoreAudioData::RatesList(d->mDeviceID);

    if (rates.empty())
    {
        // Error retrieving rates, assume 48kHz
        settings->AddSupportedRate(48000);
    }
    else
    {
        while (int rate = settings->GetNextRate())
        {
            for (auto entry : rates)
            {
                if (entry != rate)
                    continue;
                settings->AddSupportedRate(entry);
            }
        }
    }

    // Supported format: 16 bits audio or float
    settings->AddSupportedFormat(FORMAT_S16);
    settings->AddSupportedFormat(FORMAT_FLT);

    ChannelsArr channels {};
    if (!d->ChannelsList(d->mDeviceID, digital, channels))
    {
        // Error retrieving list of supported channels, assume stereo only
        settings->AddSupportedChannels(2);
    }
    else
    {
        for (int i = CHANNELS_MIN; i <= CHANNELS_MAX; i++)
        {
            if (channels[i-1])
            {
                LOG(VB_AUDIO, LOG_INFO, QString("AudioOutputCA::Support %1 channels").arg(i));
                // In case 8 channels are supported but not 6, fake 6
                if (i == 8 && !channels[6-1])
                    settings->AddSupportedChannels(6);
                settings->AddSupportedChannels(i);
            }
        }
    }

    if (d->FindAC3Stream())
    {
        settings->setPassthrough(1); // yes passthrough
    }
    return settings;
}

bool AudioOutputCA::OpenDevice()
{
    bool deviceOpened = false;

    if (d->mWasDigital)
    {
    }
    LOG(VB_AUDIO, LOG_INFO, "AudioOutputCA::OpenDevice: Entering");
    if (m_passthru || m_enc)
    {
        LOG(VB_AUDIO, LOG_INFO, "AudioOutputCA::OpenDevice() Trying Digital.");
        if (!(deviceOpened = d->OpenSPDIF()))
            d->CloseSPDIF();
    }

    if (!deviceOpened)
    {
        LOG(VB_AUDIO, LOG_INFO, "AudioOutputCA::OpenDevice() Trying Analog.");
        int result = -1;
        //for (int i=0; result < 0 && i < 10; i++)
        {
            result = d->OpenAnalog();
            LOG(VB_AUDIO, LOG_INFO, QString("AudioOutputCA::OpenDevice: OpenAnalog = %1").arg(result));
            if (result < 0)
            {
                d->CloseAnalog();
                usleep(1s - 1us); // Argument to usleep must be less than 1 second
            }
        }
        deviceOpened = (result > 0);
    }

    if (!deviceOpened)
    {
        LOG(VB_GENERAL, LOG_ERR, "AudioOutputCA Error: Couldn't open any audio device!");
        d->CloseAnalog();
        return false;
    }

    if (m_internalVol && m_setInitialVol)
    {
        QString controlLabel = gCoreContext->GetSetting("MixerControl", "PCM");
        controlLabel += "MixerVolume";
        SetCurrentVolume(gCoreContext->GetNumSetting(controlLabel, 80));
    }

    return true;
}

void AudioOutputCA::CloseDevice()
{
    LOG(VB_AUDIO, LOG_INFO, LOC + QString("CloseDevice [%1]: Entering")
            .arg(d->mDigitalInUse ? "SPDIF" : "Analog"));
    if (d->mDigitalInUse)
        d->CloseSPDIF();
    else
        d->CloseAnalog();
}

template <class AudioDataType>
static inline void _ReorderSmpteToCA(AudioDataType *buf, uint frames)
{
    AudioDataType tmpLS;
    AudioDataType tmpRS;
    AudioDataType tmpRLs;
    AudioDataType tmpRRs;
    AudioDataType *buf2;
    for (uint i = 0; i < frames; i++)
    {
        buf = buf2 = buf + 4;
        tmpRLs = *buf++;
        tmpRRs = *buf++;
        tmpLS = *buf++;
        tmpRS = *buf++;

        *buf2++ = tmpLS;
        *buf2++ = tmpRS;
        *buf2++ = tmpRLs;
        *buf2++ = tmpRRs;
    }
}

static inline void ReorderSmpteToCA(void *buf, uint frames, AudioFormat format)
{
    switch(AudioOutputSettings::FormatToBits(format))
    {
        case  8: _ReorderSmpteToCA((uchar *)buf, frames); break;
        case 16: _ReorderSmpteToCA((short *)buf, frames); break;
        default: _ReorderSmpteToCA((int   *)buf, frames); break;
    }
}

/** Object-oriented part of callback */
bool AudioOutputCA::RenderAudio(unsigned char *aubuf,
                                int size, unsigned long long timestamp)
{
    if (m_pauseAudio || m_killAudio)
    {
        m_actuallyPaused = true;
        return false;
    }

    /* This callback is called when the sound system requests
     data.  We don't want to block here, because that would
     just cause dropouts anyway, so we always return whatever
     data is available.  If we haven't received enough, either
     because we've finished playing or we have a buffer
     underrun, we play silence to fill the unused space.  */

    int written_size = GetAudioData(aubuf, size, false);
    if (written_size && (size > written_size))
    {
        // play silence on buffer underrun
        memset(aubuf + written_size, 0, size - written_size);
    }

    //Audio received is in SMPTE channel order, reorder to CA unless passthru
    if (!m_passthru && m_channels == 8)
    {
        ReorderSmpteToCA(aubuf, size / m_outputBytesPerFrame, m_outputFormat);
    }

    /* update audiotime (m_bufferedBytes is read by GetBufferedOnSoundcard) */
    UInt64 nanos = AudioConvertHostTimeToNanos(timestamp -
                                               AudioGetCurrentHostTime());
    m_bufferedBytes = (int)((nanos / 1000000000.0) *  // secs
                            (m_effDsp / 100.0) *      // frames/sec
                            m_outputBytesPerFrame);   // bytes/frame

    return (written_size > 0);
}

// unneeded and unused in CA
void AudioOutputCA::WriteAudio([[maybe_unused]] unsigned char *aubuf,
                               [[maybe_unused]] int size)
{
}

int AudioOutputCA::GetBufferedOnSoundcard(void) const
{
    return m_bufferedBytes;
}

/** Reimplement the base class's version of GetAudiotime()
 *  so that we don't use gettimeofday or Qt mutexes.
 */
std::chrono::milliseconds AudioOutputCA::GetAudiotime(void)
{
    std::chrono::milliseconds audbuf_timecode = GetBaseAudBufTimeCode();

    if (audbuf_timecode == 0ms)
        return 0ms;

    int totalbuffer = audioready() + GetBufferedOnSoundcard();

    return audbuf_timecode - millisecondsFromFloat(totalbuffer * 100000.0 /
                                   (m_outputBytesPerFrame *
                                    m_effDsp * m_stretchFactor));
}

/* This callback provides converted audio data to the default output device. */
OSStatus RenderCallbackAnalog(void                       *inRefCon,
                              AudioUnitRenderActionFlags *ioActionFlags,
                              const AudioTimeStamp       *inTimeStamp,
                              [[maybe_unused]] UInt32     inBusNumber,
                              [[maybe_unused]] UInt32     inNumberFrames,
                              AudioBufferList            *ioData)
{
    AudioOutputCA *inst = (static_cast<CoreAudioData *>(inRefCon))->mCA;

    if (!inst->RenderAudio((unsigned char *)(ioData->mBuffers[0].mData),
                           ioData->mBuffers[0].mDataByteSize,
                           inTimeStamp->mHostTime))
    {
        // play silence if RenderAudio returns false
        memset(ioData->mBuffers[0].mData, 0, ioData->mBuffers[0].mDataByteSize);
        *ioActionFlags = kAudioUnitRenderAction_OutputIsSilence;
    }
    return noErr;
}

int AudioOutputCA::GetVolumeChannel([[maybe_unused]] int channel) const
{
    // FIXME: this only returns global volume
    Float32 volume;

    if (!AudioUnitGetParameter(d->mOutputUnit,
                               kHALOutputParam_Volume,
                               kAudioUnitScope_Global, 0, &volume))
        return (int)lroundf(volume * 100.0F);

    return 0;    // error case
}

void AudioOutputCA::SetVolumeChannel([[maybe_unused]] int channel, int volume)
{
    // FIXME: this only sets global volume
    AudioUnitSetParameter(d->mOutputUnit, kHALOutputParam_Volume,
                          kAudioUnitScope_Global, 0, (volume * 0.01F), 0);
}

// IOProc style callback for SPDIF audio output
static OSStatus RenderCallbackSPDIF([[maybe_unused]] AudioDeviceID        inDevice,
                                    [[maybe_unused]] const AudioTimeStamp *inNow,
                                    [[maybe_unused]] const void           *inInputData,
                                    [[maybe_unused]] const AudioTimeStamp *inInputTime,
                                    AudioBufferList      *outOutputData,
                                    const AudioTimeStamp *inOutputTime,
                                    void                 *inRefCon)
{
    auto *d = static_cast<CoreAudioData *>(inRefCon);
    AudioOutputCA    *a = d->mCA;
    int           index = d->mStreamIndex;

    /*
     * HACK: No idea why this would be the case, but after the second run, we get
     * incorrect value
     */
    if (d->mBytesPerPacket > 0 &&
        outOutputData->mBuffers[index].mDataByteSize > d->mBytesPerPacket)
    {
        outOutputData->mBuffers[index].mDataByteSize = d->mBytesPerPacket;
    }
    if (!a->RenderAudio((unsigned char *)(outOutputData->mBuffers[index].mData),
                        outOutputData->mBuffers[index].mDataByteSize,
                        inOutputTime->mHostTime))
    {
        // play silence if RenderAudio returns false
        memset(outOutputData->mBuffers[index].mData, 0,
              outOutputData->mBuffers[index].mDataByteSize);
    }
    return noErr;
}

CoreAudioData::CoreAudioData(AudioOutputCA *parent) : mCA(parent)
{
    // Reset all the devices to a default 'non-hog' and mixable format.
    // If we don't do this we may be unable to find the Default Output device.
    // (e.g. if we crashed last time leaving it stuck in AC-3 mode)
    ResetAudioDevices();

    mDeviceID = GetDefaultOutputDevice();
}

CoreAudioData::CoreAudioData(AudioOutputCA *parent, AudioDeviceID deviceID) :
    mCA(parent)
{
    ResetAudioDevices();
    mDeviceID = deviceID;
}

CoreAudioData::CoreAudioData(AudioOutputCA *parent, QString deviceName) :
    mCA(parent)
{
    ResetAudioDevices();
    mDeviceID = GetDeviceWithName(deviceName);
    if (!mDeviceID)
    {
        // Didn't find specified device, use default one
        mDeviceID = GetDefaultOutputDevice();
        if (deviceName != "Default Output Device")
        {
            LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:CoreAudioData: \"%1\" not found, using default device %2.")
                 .arg(deviceName).arg(mDeviceID));
        }
    }
    LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::CoreAudioData: device number is %1")
          .arg(mDeviceID));
}

AudioDeviceID CoreAudioData::GetDeviceWithName(const QString &deviceName)
{
    UInt32 size = 0;
    AudioDeviceID deviceID = 0;
    AudioObjectPropertyAddress pa
    {
	kAudioHardwarePropertyDevices,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa,
						  0, nullptr, &size);
    if (err)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:GetPropertyDataSize: Unable to retrieve the property sizes. "
                     "Error [%1]")
             .arg(err));
	return deviceID;
    }

    UInt32 deviceCount = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devices = {};
    devices.resize(deviceCount);

    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa,
                                    0, nullptr, &size, devices.data());
    if (err)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:GetDeviceWithName: Unable to retrieve the list of available devices. "
                     "Error [%1]")
             .arg(err));
    }
    else
    {
        for (const auto & dev : devices)
        {
            CoreAudioData device(nullptr, dev);
            if (device.GetTotalOutputChannels() == 0)
                continue;
            QString *name = device.GetName();
            if (name && *name == deviceName)
            {
                LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::GetDeviceWithName: Found: %1").arg(*name));
                deviceID = dev;
                delete name;
            }
            if (deviceID)
                break;
        }
    }
    return deviceID;
}

AudioDeviceID CoreAudioData::GetDefaultOutputDevice()
{
    UInt32        paramSize;
    AudioDeviceID deviceId = 0;
    AudioObjectPropertyAddress pa
    {
	kAudioHardwarePropertyDefaultOutputDevice,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    // Find the ID of the default Device
    paramSize = sizeof(deviceId);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa,
					      0, nullptr, &paramSize, &deviceId);
    if (err == noErr)
        LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::GetDefaultOutputDevice: default device ID = %1").arg(deviceId));
    else
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:GetDefaultOutputDevice: could not get default audio device: [%1]")
             .arg(OSS_STATUS(err)));
        deviceId = 0;
    }
    return deviceId;
}

int CoreAudioData::GetTotalOutputChannels()
{
    if (!mDeviceID)
        return 0;
    UInt32 channels = 0;
    UInt32 size = 0;
    AudioObjectPropertyAddress pa
    {
	kAudioDevicePropertyStreamConfiguration,
	kAudioDevicePropertyScopeOutput, // Scope needs to be set to output to find output streams
	kMythAudioObjectPropertyElementMain
    };

    OSStatus err = AudioObjectGetPropertyDataSize(mDeviceID, &pa,
						  0, nullptr, &size);
    if (err)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:GetTotalOutputChannels: Unable to get "
                     "size of device output channels - id: %1 Error = [%2]")
             .arg(mDeviceID)
             .arg(err));
	return 0;
    }

    auto *pList = (AudioBufferList *)malloc(size);
    err = AudioObjectGetPropertyData(mDeviceID, &pa,
				     0, nullptr, &size, pList);
    if (!err)
    {
        for (UInt32 buffer = 0; buffer < pList->mNumberBuffers; buffer++)
            channels += pList->mBuffers[buffer].mNumberChannels;
    }
    else
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:GetTotalOutputChannels: Unable to get "
                     "total device output channels - id: %1 Error = [%2]")
             .arg(mDeviceID)
             .arg(err));
    }
    LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::GetTotalOutputChannels: Found %1 channels in %2 buffers")
          .arg(channels).arg(pList->mNumberBuffers));
    free(pList);
    return channels;
}

QString *CoreAudioData::GetName()
{
    if (!mDeviceID)
        return nullptr;

    AudioObjectPropertyAddress pa
    {
	kAudioObjectPropertyName,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    CFStringRef name;
    UInt32 propertySize = sizeof(CFStringRef);
    OSStatus err = AudioObjectGetPropertyData(mDeviceID, &pa,
                                              0, nullptr, &propertySize, &name);
    if (err)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:AudioObjectGetPropertyData for kAudioObjectPropertyName error: [%1]")
              .arg(err));
        return nullptr;
    }
    char *cname = new char[CFStringGetLength(name) + 1];
    CFStringGetCString(name, cname, CFStringGetLength(name) + 1, kCFStringEncodingUTF8);
    auto *qname = new QString(cname);
    delete[] cname;
    return qname;
}

bool CoreAudioData::GetAutoHogMode()
{
    UInt32 val = 0;
    UInt32 size = sizeof(val);
    AudioObjectPropertyAddress pa
    {
	kAudioHardwarePropertyHogModeIsAllowed,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr, &size, &val);
    if (err)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:GetAutoHogMode: Unable to get auto 'hog' mode. Error = [%1]")
             .arg(err));
        return false;
    }
    return (val == 1);
}

void CoreAudioData::SetAutoHogMode(bool enable)
{
    UInt32 val = enable ? 1 : 0;
    AudioObjectPropertyAddress pa
    {
	kAudioHardwarePropertyHogModeIsAllowed,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    OSStatus err = AudioObjectSetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr,
					      sizeof(val), &val);
    if (err)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:SetAutoHogMode: Unable to set auto 'hog' mode. Error = [%1]")
             .arg(err));
    }
}

pid_t CoreAudioData::GetHogStatus()
{
    pid_t PID;
    UInt32 PIDsize = sizeof(PID);
    AudioObjectPropertyAddress pa
    {
	kAudioDevicePropertyHogMode,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr,
                                 &PIDsize, &PID);
    if (err != noErr)
    {
        // This is not a fatal error.
        // Some drivers simply don't support this property
        LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::GetHogStatus: unable to check: [%1]")
              .arg(err));
        return -1;
    }
    return PID;
}

bool CoreAudioData::SetHogStatus(bool hog)
{
    AudioObjectPropertyAddress pa
    {
	kAudioDevicePropertyHogMode,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    // According to Jeff Moore (Core Audio, Apple), Setting kAudioDevicePropertyHogMode
    // is a toggle and the only way to tell if you do get hog mode is to compare
    // the returned pid against getpid, if the match, you have hog mode, if not you don't.
    if (!mDeviceID)
        return false;

    if (hog)
    {
        if (mHog == -1) // Not already set
        {
            LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::SetHogStatus: Setting 'hog' status on device %1")
                  .arg(mDeviceID));
	    OSStatus err = AudioObjectSetPropertyData(mDeviceID, &pa, 0, nullptr,
						      sizeof(mHog), &mHog);
            if (err || mHog != getpid())
            {
                LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:SetHogStatus: Unable to set 'hog' status. Error = [%1]")
                     .arg(OSS_STATUS(err)));
                return false;
            }
            LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::SetHogStatus: Successfully set 'hog' status on device %1")
                  .arg(mDeviceID));
        }
    }
    else
    {
        if (mHog > -1) // Currently Set
        {
            LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::SetHogStatus: Releasing 'hog' status on device %1")
                  .arg(mDeviceID));
            pid_t hogPid = -1;
	    OSStatus err = AudioObjectSetPropertyData(mDeviceID, &pa, 0, nullptr,
						      sizeof(hogPid), &hogPid);
            if (err || hogPid == getpid())
            {
                LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:SetHogStatus: Unable to release 'hog' status. Error = [%1]")
                     .arg(OSS_STATUS(err)));
                return false;
            }
            mHog = hogPid; // Reset internal state
        }
    }
    return true;
}

bool CoreAudioData::SetMixingSupport(bool mix)
{
    if (!mDeviceID)
        return false;
    int restore = -1;
    if (mMixerRestore == -1) // This is our first change to this setting. Store the original setting for restore
        restore = (GetMixingSupport() ? 1 : 0);
    UInt32 mixEnable = mix ? 1 : 0;
    LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::SetMixingSupport: %1abling mixing for device %2")
          .arg(mix ? "En" : "Dis")
          .arg(mDeviceID));

    AudioObjectPropertyAddress pa
    {
	kAudioDevicePropertySupportsMixing,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };
    OSStatus err = AudioObjectSetPropertyData(mDeviceID, &pa, 0, nullptr,
					      sizeof(mixEnable), &mixEnable);
    if (err)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:SetMixingSupport: Unable to set MixingSupport to %1. Error = [%2]")
             .arg(mix ? "'On'" : "'Off'")
             .arg(OSS_STATUS(err)));
        return false;
    }
    if (mMixerRestore == -1)
        mMixerRestore = restore;
    return true;
}

bool CoreAudioData::GetMixingSupport()
{
    if (!mDeviceID)
        return false;
    UInt32 val = 0;
    UInt32 size = sizeof(val);
    AudioObjectPropertyAddress pa
    {
	kAudioDevicePropertySupportsMixing,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };
    OSStatus err = AudioObjectGetPropertyData(mDeviceID, &pa, 0, nullptr,
					      &size, &val);
    if (err)
        return false;
    return (val > 0);
}

/**
 * Get a list of all the streams on this device
 */
AudioStreamIDVec CoreAudioData::StreamsList(AudioDeviceID d)
{
    OSStatus       err;
    UInt32         listSize;
    AudioStreamIDVec vec {};

    AudioObjectPropertyAddress pa
    {
	kAudioDevicePropertyStreams,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    err = AudioObjectGetPropertyDataSize(d, &pa,
					 0, nullptr, &listSize);
    if (err != noErr)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:StreamsList: could not get list size: [%1]")
              .arg(OSS_STATUS(err)));
        return {};
    }
    try
    {
        // Bugfix: vec.reserve will not change size of vector since contents are updated directly via memory copy
        // In general all std::vector arrays have been changed from reserve to resize.
        vec.resize(listSize / sizeof(AudioStreamID));
    }
    catch (...)
    {
        LOG(VB_GENERAL, LOG_ERR, "CoreAudioData Error:StreamsList(): out of memory?");
        return {};
    }

    err = AudioObjectGetPropertyData(d, &pa,
				     0, nullptr, &listSize, vec.data());
    if (err != noErr)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:StreamsList: could not get list: [%1]")
              .arg(OSS_STATUS(err)));
        return {};
    }

    return vec;
}

AudioStreamRangedVec CoreAudioData::FormatsList(AudioStreamID s)
{
    OSStatus                     err;
    AudioStreamRangedVec         vec;
    UInt32                       listSize;

    AudioObjectPropertyAddress pa
    {
        // Bugfix: kAudioStreamPropertyPhysicalFormats, is meant to only give array of AudioStreamBasicDescription
        kAudioStreamPropertyAvailablePhysicalFormats, // gives array of AudioStreamRangedDescription
        kAudioObjectPropertyScopeGlobal,
        kMythAudioObjectPropertyElementMain
    };

    // Retrieve all the stream formats supported by this output stream
    err = AudioObjectGetPropertyDataSize(s, &pa, 0, nullptr, &listSize);
    if (err != noErr)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:FormatsList(): couldn't get list size: [%1]")
             .arg(OSS_STATUS(err)));
        return {};
    }

    try
    {
        vec.resize(listSize / sizeof(AudioStreamRangedDescription));
    }
    catch (...)
    {
        LOG(VB_GENERAL, LOG_ERR, "CoreAudioData Error:FormatsList(): out of memory?");
        return {};
    }

    err = AudioObjectGetPropertyData(s, &pa, 0, nullptr, &listSize, vec.data());
    if (err != noErr)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:FormatsList: couldn't get list: [%1]")
             .arg(OSS_STATUS(err)));
        return {};
    }

    return vec;
}

static UInt32   sNumberCommonSampleRates = 15;
static Float64  sCommonSampleRates[] = {
    8000.0,   11025.0,  12000.0,
    16000.0,  22050.0,  24000.0,
    32000.0,  44100.0,  48000.0,
    64000.0,  88200.0,  96000.0,
    128000.0, 176400.0, 192000.0 };

static bool IsRateCommon(Float64 inRate)
{
    bool theAnswer = false;
    for(UInt32 i = 0; !theAnswer && (i < sNumberCommonSampleRates); i++)
    {
        theAnswer = inRate == sCommonSampleRates[i];
    }
    return theAnswer;
}

RatesVec CoreAudioData::RatesList(AudioDeviceID d)
{
    OSStatus                    err;
    AudioValueRangeVec          ranges;
    RatesVec                    finalvec;
    UInt32                      listSize;

    AudioObjectPropertyAddress pa
    {
	kAudioDevicePropertyAvailableNominalSampleRates,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    // retrieve size of rate list
    err = AudioObjectGetPropertyDataSize(d, &pa, 0, nullptr, &listSize);
    if (err != noErr)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:RatesList(): couldn't get data rate list size: [%1]")
             .arg(err));
        return {};
    }

    try
    {
        ranges.resize(listSize / sizeof(AudioValueRange));
        finalvec.reserve(listSize / sizeof(AudioValueRange));
    }
    catch (...)
    {
        LOG(VB_GENERAL, LOG_ERR, "CoreAudioData Error:RatesList(): out of memory?");
        return {};
    }

    err = AudioObjectGetPropertyData(d, &pa, 0, nullptr, &listSize, ranges.data());
    if (err != noErr)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:RatesList(): couldn't get list: [%1]")
             .arg(err));
        return {};
    }

    // iterate through the ranges and add the minimum, maximum, and common rates in between
    UInt32 theFirstIndex;
    UInt32 theLastIndex = 0;
    for(auto range : ranges)
    {
        theFirstIndex = theLastIndex;
        // find the index of the first common rate greater than or equal to the minimum
        while((theFirstIndex < sNumberCommonSampleRates) &&  (sCommonSampleRates[theFirstIndex] < range.mMinimum))
            theFirstIndex++;

        if (theFirstIndex >= sNumberCommonSampleRates)
            break;

        theLastIndex = theFirstIndex;
        // find the index of the first common rate greater than or equal to the maximum
        while((theLastIndex < sNumberCommonSampleRates) && (sCommonSampleRates[theLastIndex] < range.mMaximum))
        {
	    finalvec.push_back(sCommonSampleRates[theLastIndex]);
            theLastIndex++;
        }
        if (IsRateCommon(range.mMinimum))
	    finalvec.push_back(range.mMinimum);
        else if (IsRateCommon(range.mMaximum))
            finalvec.push_back(range.mMaximum);
    }

    return finalvec;
}

bool CoreAudioData::ChannelsList(AudioDeviceID /*d*/, bool passthru, ChannelsArr& chans)
{
    AudioStreamIDVec            streams;
    AudioStreamRangedVec        formats;
    bool                        founddigital = false;

    chans.fill(false);

    streams = StreamsList(mDeviceID);
    if (streams.empty())
        return false;

    if (passthru)
    {
        for (auto stream : streams)
        {
            formats = FormatsList(stream);
            if (formats.empty())
                continue;

            // Find a stream with a cac3 stream
            for (auto format : formats)
            {
                LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::ChannelsList: (passthru) found format: %1")
                    .arg(StreamDescriptionToString(format.mFormat)));
                // Add supported number of channels
                if (format.mFormat.mChannelsPerFrame <= CHANNELS_MAX)
                    chans[format.mFormat.mChannelsPerFrame-1] = true;

                if (format.mFormat.mFormatID == 'IAC3' ||
                    format.mFormat.mFormatID == kAudioFormat60958AC3)
                {
                    // By default AC3 has 6 (5.1) channels but CoreAudio seems to set mChannelsPerFrame to 2 
                    // and considers it a "2 channel Encoded Digital Audio"
                    chans[6-1] = true;
                    //	chans[format.mFormat.mChannelsPerFrame-1] = true;
                    founddigital = true;
                }
            }
        }
    }

    if (!founddigital)
    {
        for (auto stream : streams)
        {
            formats = FormatsList(stream);
            if (formats.empty())
                continue;
            for (auto format : formats)
            {
                LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::ChannelsList: (!founddigital) found format: %1")
                    .arg(StreamDescriptionToString(format.mFormat)));
                if (format.mFormat.mChannelsPerFrame <= CHANNELS_MAX)
                    chans[format.mFormat.mChannelsPerFrame-1] = true;
            }
        }
    }
    return true;
}

// Structure to hold channel label arrays
typedef struct {
    AudioChannelLabel* labels;
    UInt32 count;
} ChannelLabels;

// Function to extract channel labels from layout
OSStatus ExtractChannelLabels(const AudioChannelLayout* layout, ChannelLabels* outLabels) {
    if (layout->mNumberChannelDescriptions > 0) {
        // Layout uses explicit channel descriptions
        outLabels->count = layout->mNumberChannelDescriptions;
        outLabels->labels = (AudioChannelLabel *)malloc(outLabels->count * sizeof(AudioChannelLabel));

        if (!outLabels->labels) return kAudio_MemFullError;

        for (UInt32 i = 0; i < outLabels->count; i++) {
            outLabels->labels[i] = layout->mChannelDescriptions[i].mChannelLabel;
        }
    } else {
        // Expand standard layout tag to get channel descriptions
        UInt32 propertySize = 0;
        OSStatus status = AudioFormatGetPropertyInfo(
            kAudioFormatProperty_ChannelLayoutForTag,
            sizeof(layout->mChannelLayoutTag),
            &layout->mChannelLayoutTag,
            &propertySize
        );

        if (status != noErr) return status;

        AudioChannelLayout* expanded = (AudioChannelLayout*)malloc(propertySize);
        if (!expanded) return kAudio_MemFullError;

        status = AudioFormatGetProperty(
            kAudioFormatProperty_ChannelLayoutForTag,
            sizeof(layout->mChannelLayoutTag),
            &layout->mChannelLayoutTag,
            &propertySize,
            expanded
        );

        if (status != noErr) {
            free(expanded);
            return status;
        }

        if (expanded->mNumberChannelDescriptions > 0) {
            outLabels->count = expanded->mNumberChannelDescriptions;
            outLabels->labels = (AudioChannelLabel*) malloc(outLabels->count * sizeof(AudioChannelLabel));

            if (!outLabels->labels) {
                free(expanded);
                return kAudio_MemFullError;
            }

            for (UInt32 i = 0; i < expanded->mNumberChannelDescriptions; i++) {
                outLabels->labels[i] = expanded->mChannelDescriptions[i].mChannelLabel;
            }
        } else {
            // Fallback for layouts with no descriptions
            outLabels->count = 0;
        }

        free(expanded);
    }

    return noErr;
}

// Generate channel map by comparing layouts
OSStatus CreateChannelMap(const AudioChannelLayout* standard,
                          const AudioChannelLayout* custom,
                          SInt32** outMap,
                          UInt32* outCount) {
    ChannelLabels stdLabels, customLabels;

    // Extract labels from both layouts
    OSStatus status;
    if ((status = ExtractChannelLabels(standard, &stdLabels)) != noErr) return status;
    if ((status = ExtractChannelLabels(custom, &customLabels)) != noErr) {
        free(stdLabels.labels);
        return status;
    }

    // Create channel map array
    *outCount = customLabels.count;
    *outMap = (SInt32*)malloc(customLabels.count * sizeof(SInt32));

    if (!*outMap) {
        free(stdLabels.labels);
        free(customLabels.labels);
        return kAudio_MemFullError;
    }

    // Initialize with silence (-1)
    for (UInt32 i = 0; i < customLabels.count; i++) {
        (*outMap)[i] = -1;
    }

    // Find matching channels
    for (UInt32 customIdx = 0; customIdx < customLabels.count; customIdx++) {
        for (UInt32 stdIdx = 0; stdIdx < stdLabels.count; stdIdx++) {
            if (customLabels.labels[customIdx] == stdLabels.labels[stdIdx]) {
                (*outMap)[customIdx] = (SInt32)stdIdx;
                break;  // Use first match
            }
        }
    }

    // Cleanup
    free(stdLabels.labels);
    free(customLabels.labels);

    return noErr;
}

int CoreAudioData::OpenAnalog()
{
    AudioComponentDescription    desc;
    AudioStreamBasicDescription  DeviceFormat;
    AudioChannelLayout          *layout;
    AudioDeviceID                defaultDevice = GetDefaultOutputDevice();

    LOG(VB_AUDIO, LOG_INFO, "CoreAudioData::OpenAnalog: Entering");

    desc.componentType = kAudioUnitType_Output;
    if (defaultDevice == mDeviceID)
    {
        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    }
    else
    {
        desc.componentSubType = kAudioUnitSubType_HALOutput;
    }
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;
    mDigitalInUse = false;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr)
    {
        LOG(VB_GENERAL, LOG_ERR, "CoreAudioData Error:OpenAnalog: AudioComponentFindNext failed");
        return false;
    }

    OSErr err = AudioComponentInstanceNew(comp, &mOutputUnit);
    if (err)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:OpenAnalog: AudioComponentInstanceNew returned %1")
              .arg(err));
        return false;
    }

    // Check if we have IO
    UInt32 hasIO      = 0;
    UInt32 size_hasIO = sizeof(hasIO);
    err = AudioUnitGetProperty(mOutputUnit,
                               kAudioOutputUnitProperty_HasIO,
                               kAudioUnitScope_Output,
                               0,
                               &hasIO, &size_hasIO);
    LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::OpenAnalog: HasIO (output) = %1").arg(hasIO));
    if (!hasIO)
    {
        UInt32 enableIO = 1;
        err = AudioUnitSetProperty(mOutputUnit,
                                   kAudioOutputUnitProperty_EnableIO,
                                   kAudioUnitScope_Global,
                                   0,
                                   &enableIO, sizeof(enableIO));
        if (err)
        {
            LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:OpenAnalog: failed enabling IO: %1")
                 .arg(err));
        }
        hasIO = 0;
        err = AudioUnitGetProperty(mOutputUnit,
                                   kAudioOutputUnitProperty_HasIO,
                                   kAudioUnitScope_Output,
                                   0,
                                   &hasIO, &size_hasIO);
        LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::HasIO = %1").arg(hasIO));
    }

    /*
     * We shouldn't have to do this distinction, however for some unknown reasons
     * assigning device to AudioUnit fail when switching from SPDIF mode
     */
    if (defaultDevice != mDeviceID)
    {
        err = AudioUnitSetProperty(mOutputUnit,
                                   kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Global,
                                   0,
                                   &mDeviceID, sizeof(mDeviceID));
        if (err)
        {
            LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:OpenAnalog: Unable to set current device to %1. Error = %2")
                  .arg(mDeviceID)
                  .arg(err));
            return -1;
        }
    }
    /* Get the current format */
    UInt32 param_size = sizeof(AudioStreamBasicDescription);

    err = AudioUnitGetProperty(mOutputUnit,
                               kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input,
                               0,
                               &DeviceFormat,
                               &param_size );
    if (err)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:OpenAnalog: Unable to retrieve current stream format: [%1]")
              .arg(err));
    }
    else
    {
        LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::OpenAnalog: current format is: %1")
              .arg(StreamDescriptionToString(DeviceFormat)));
    }
    /* Get the channel layout of the device side of the unit */
    Boolean bWritable;
    Boolean bLayoutSetUsingChannelMap = false;
    err = AudioUnitGetPropertyInfo(mOutputUnit,
                           kAudioUnitProperty_AudioChannelLayout,
                           kAudioUnitScope_Output,
                           0,
                           &param_size,
                           &bWritable);
    if (err == noErr)
    {
        layout = (AudioChannelLayout *) malloc(param_size);

	    err = AudioUnitGetProperty(mOutputUnit,
                            kAudioUnitProperty_AudioChannelLayout,
                            kAudioUnitScope_Output,
                            0,
                            layout,
                            &param_size);

        /* We need to "fill out" the ChannelLayout, because there are multiple ways that it can be set */
        if(layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap)
        {
            /* bitmap defined channellayout */
            err = AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForBitmap,
                                         sizeof(UInt32), &layout->mChannelBitmap,
                                         &param_size,
                                         layout);
            if (err)
                LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:OpenAnalog: Can't retrieve bitmap channel layout Error = %1")
                .arg(err));
        }
        if(layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions )
        {
            /* layouttags defined channellayout */
            err = AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForTag,
                                         sizeof(AudioChannelLayoutTag),
                                         &layout->mChannelLayoutTag,
                                         &param_size,
                                         layout);
            if (err)
                LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:OpenAnalog: Can't retrieve bitmap channel layout Error = %1")
                .arg(err));
        }

        // layout->mChannelLayoutTag should now be kAudioChannelLayoutTag_UseChannelDescriptions
        LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::OpenAnalog: Layout of AUHAL has %1 channels")
              .arg(layout->mNumberChannelDescriptions));

        QString channelMapDescription; // String representation of channelMap

        int channels_found = 0;
        for(UInt32 i = 0; i < layout->mNumberChannelDescriptions; i++)
        {

            switch( layout->mChannelDescriptions[i].mChannelLabel)
            {
                case kAudioChannelLabel_Left:
                    channelMapDescription.append("L");
                    channels_found++;
                    break;
                case kAudioChannelLabel_Right:
                    channelMapDescription.append("R");
                    channels_found++;
                    break;
                case kAudioChannelLabel_Center:
                    channelMapDescription.append("C");
                    channels_found++;
                    break;
                case kAudioChannelLabel_LFEScreen:
                    channelMapDescription.append("LFE");
                    channels_found++;
                    break;
                case kAudioChannelLabel_LeftSurround:
                    channelMapDescription.append("Ls");
                    channels_found++;
                    break;
                case kAudioChannelLabel_RightSurround:
                    channelMapDescription.append("Rs");
                    channels_found++;
                    break;
                case kAudioChannelLabel_RearSurroundLeft:
                    channelMapDescription.append("Rls");
                    channels_found++;
                    break;
                case kAudioChannelLabel_RearSurroundRight:
                    channelMapDescription.append("Rrs");
                    channels_found++;
                    break;
                case kAudioChannelLabel_CenterSurround:
                    channelMapDescription.append("Cs");
                    channels_found++;
                    break;
                case kAudioChannelLabel_LeftCenter:
                    channelMapDescription.append("Lc");
                    channels_found++;
                    break;
                case kAudioChannelLabel_RightCenter:
                    channelMapDescription.append("Rc");
                    channels_found++;
                    break;
                default:
                    // There are many more channels but right now we seem to only cater for up to 7.1
                    channelMapDescription.append("?");
                    LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::unrecognized channel form provided by driver: %1")
                        .arg(layout->mChannelDescriptions[i].mChannelLabel));
            }
            if (i < layout->mNumberChannelDescriptions - 1) {
                channelMapDescription.append(" "); // for readability
            }
        }
        LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::OpenAnalog: Channel map found: ").append(channelMapDescription));
        if(channels_found == 0)
        {
            LOG(VB_GENERAL, LOG_WARNING, "CoreAudioData Warning:Audio device is not configured. "
                 "You should configure your speaker layout with "
                 "the \"Audio Midi Setup\" utility in /Applications/"
                 "Utilities.");
        }
        else
        {
            // Set the channelMap directly. In certain circumstances the channel order may not be expressible
            // with a kAudioChannelLayoutTag (eg. HDMI can swap the C and LFE channel order in 5.1 due to ambiguous standards)
            // so we need to cross check against the standard channel order

            AudioChannelLayout* standardLayout = (AudioChannelLayout*) calloc(1, sizeof(AudioChannelLayout));
            switch(layout->mNumberChannelDescriptions)
            {
                case 1:
                    standardLayout->mChannelLayoutTag = kAudioChannelLayoutTag_Mono;
                    break;
                case 2:
                    standardLayout->mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
                    break;
                case 6:
                    // L   R   C    LFE  LS   RS
                    standardLayout->mChannelLayoutTag = kAudioChannelLayoutTag_AudioUnit_5_1;
                    break;
                case 8:
                    // L R C LFE Ls Rs Lc Rc
                    standardLayout->mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_7_1_A;
                    break;
            }

            SInt32* channelMap = nullptr;
            UInt32 mapCount = 0;

            err = CreateChannelMap(standardLayout, layout, &channelMap, &mapCount);

            if (err != noErr)
            {
                LOG(VB_AUDIO, LOG_WARNING, "CoreAudioData Warning:Audio device cannot be configured using a channel map.");
            }
            else
            {
                for (UInt32 i=0; i<mapCount; i++) {
                    LOG(VB_AUDIO, LOG_DEBUG, QString("ChannelMap[%1] %2").arg(i).arg(channelMap[i]));
                }
                err = AudioUnitSetProperty( mOutputUnit,
                                        kAudioOutputUnitProperty_ChannelMap,
                                        kAudioUnitScope_Input,
                                        0,
                                        channelMap,
                                        mapCount * sizeof(SInt32));
                if (err)
                {
                    LOG(VB_AUDIO, LOG_WARNING, QString("CoreAudioData Warning:OpenAnalog: couldn't set channels layout using kAudioOutputUnitProperty_ChannelMap [%1]")
                        .arg(err));
                }
                else {
                    LOG(VB_GENERAL, LOG_INFO, "CoreAudioData::OpenAnalog: channels layout was set using kAudioOutputUnitProperty_ChannelMap");
                    bLayoutSetUsingChannelMap = true;
                }
            }
            free(standardLayout);
            free(channelMap);
        }
        free(layout);
    }
    else
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:this driver does not support kAudioUnitProperty_AudioChannelLayout Error = %1")
        .arg(err));
    }

    // Use standard layout if unable to acquire layout to set the channel map directly
    if (!bLayoutSetUsingChannelMap)  {
        param_size = sizeof(AudioChannelLayout);
        AudioChannelLayout *p_new_layout = (AudioChannelLayout*) calloc (1, param_size);
        switch(mCA->m_channels)
        {
            case 1:
                p_new_layout->mChannelLayoutTag = kAudioChannelLayoutTag_Mono;
                break;
            case 2:
                p_new_layout->mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
                break;
            case 6:
                //  3F2-LFE        L   R   C    LFE  LS   RS
                p_new_layout->mChannelLayoutTag = kAudioChannelLayoutTag_AudioUnit_5_1;
                break;
            case 8:
                // We need
                // 3F4-LFE        L   R   C    LFE  Rls  Rrs  LS   RS
                // but doesn't exist, so we'll swap channels later
                p_new_layout->mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_7_1_A; // L R C LFE Ls Rs Lc Rc
                break;
        }
        // Set p_new_layout as the layout */
        err = AudioUnitSetProperty(mOutputUnit,
                                kAudioUnitProperty_AudioChannelLayout,
                                kAudioUnitScope_Input,
                                0,
                                p_new_layout, param_size);
        if (err)
        {
            LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:OpenAnalog: couldn't set channels layout [%1]")
                .arg(err));
        }
        else
        {
            LOG(VB_GENERAL, LOG_INFO, "CoreAudioData::OpenAnalog: channels layout was set using kAudioUnitProperty_AudioChannelLayout");
        }
        free(p_new_layout);
    }

    // Set up the audio output unit
    int formatFlags;
    switch (mCA->m_outputFormat)
    {
        case FORMAT_S16:
            formatFlags = kLinearPCMFormatFlagIsSignedInteger;
            break;
        case FORMAT_FLT:
            formatFlags = kLinearPCMFormatFlagIsFloat;
            break;
        default:
            formatFlags = kLinearPCMFormatFlagIsSignedInteger;
            break;
    }

    AudioStreamBasicDescription conv_in_desc;
    memset(&conv_in_desc, 0, sizeof(AudioStreamBasicDescription));
    conv_in_desc.mSampleRate       = mCA->m_sampleRate;
    conv_in_desc.mFormatID         = kAudioFormatLinearPCM;
    conv_in_desc.mFormatFlags      = formatFlags |
        kAudioFormatFlagsNativeEndian |
        kLinearPCMFormatFlagIsPacked;
    conv_in_desc.mBytesPerPacket   = mCA->m_outputBytesPerFrame;
    // This seems inefficient, does it hurt if we increase this?
    conv_in_desc.mFramesPerPacket  = 1;
    conv_in_desc.mBytesPerFrame    = mCA->m_outputBytesPerFrame;
    conv_in_desc.mChannelsPerFrame = mCA->m_channels;
    conv_in_desc.mBitsPerChannel   =
        AudioOutputSettings::FormatToBits(mCA->m_outputFormat);

    /* Set AudioUnit input format */
    err = AudioUnitSetProperty(mOutputUnit,
                               kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input,
                               0,
                               &conv_in_desc,
                               sizeof(AudioStreamBasicDescription));
    if (err)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:OpenAnalog: AudioUnitSetProperty returned [%1]")
              .arg(err));
        return false;
    }
    LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::OpenAnalog: set format as %1")
          .arg(StreamDescriptionToString(conv_in_desc)));
    /* Retrieve actual format */
    err = AudioUnitGetProperty(mOutputUnit,
                               kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input,
                               0,
                               &DeviceFormat,
                               &param_size);

    LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::OpenAnalog: the actual set AU format is %1")
          .arg(StreamDescriptionToString(DeviceFormat)));

    // Attach callback to default output
    AURenderCallbackStruct input;
    input.inputProc = RenderCallbackAnalog;
    input.inputProcRefCon = this;

    err = AudioUnitSetProperty(mOutputUnit,
                               kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input,
                               0, &input, sizeof(input));
    if (err)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:OpenAnalog: AudioUnitSetProperty (callback) returned [%1]")
              .arg(err));
        return false;
    }
    mIoProc = true;

    // We're all set up - start the audio output unit
    ComponentResult res = AudioUnitInitialize(mOutputUnit);
    if (res)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:OpenAnalog: AudioUnitInitialize error: [%1]")
              .arg(res));
        return false;
    }
    mInitialized = true;

    err = AudioOutputUnitStart(mOutputUnit);
    if (err)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:OpenAnalog: AudioOutputUnitStart error: [%1]")
              .arg(err));
        return false;
    }
    mStarted = true;
    return true;
}

void CoreAudioData::CloseAnalog()
{
    OSStatus err;

    LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::CloseAnalog: Entering: %1")
          .arg((long)mOutputUnit));
    if (mOutputUnit)
    {
        if (mStarted)
        {
            err = AudioOutputUnitStop(mOutputUnit);
            LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::CloseAnalog: AudioOutputUnitStop %1")
                  .arg(err));
        }
        if (mInitialized)
        {
            err = AudioUnitUninitialize(mOutputUnit);
            LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::CloseAnalog: AudioUnitUninitialize %1")
                  .arg(err));
        }
        err = AudioComponentInstanceDispose(mOutputUnit);
        LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::CloseAnalog: CloseComponent %1")
              .arg(err));
        mOutputUnit = nullptr;
    }
    mIoProc = false;
    mInitialized = false;
    mStarted = false;
    mWasDigital = false;
}

bool CoreAudioData::OpenSPDIF()
{
    OSStatus       err;
    AudioStreamIDVec streams;
    AudioStreamBasicDescription outputFormat {};

    LOG(VB_AUDIO, LOG_INFO, "CoreAudioData::OpenSPDIF: Entering");

    streams = StreamsList(mDeviceID);
    if (streams.empty())
    {
        LOG(VB_GENERAL, LOG_WARNING, "CoreAudioData Warning:OpenSPDIF: Couldn't retrieve list of streams");
        return false;
    }

    for (size_t i = 0; i < streams.size(); ++i)
    {
        AudioStreamRangedVec formats = FormatsList(streams[i]);
        if (formats.empty())
            continue;

        // Find a stream with a cac3 stream
        for (auto format : formats)
        {
            LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::OpenSPDIF: Considering Physical Format: %1")
                  .arg(StreamDescriptionToString(format.mFormat)));
            if ((format.mFormat.mFormatID == 'IAC3' ||
                 format.mFormat.mFormatID == kAudioFormat60958AC3) &&
                format.mFormat.mSampleRate == mCA->m_sampleRate)
            {
                LOG(VB_AUDIO, LOG_INFO, "CoreAudioData::OpenSPDIF: Found digital format");
                mStreamIndex  = i;
                mStreamID     = streams[i];
                outputFormat  = format.mFormat;
                break;
            }
        }
        if (outputFormat.mFormatID)
            break;
    }

    if (!outputFormat.mFormatID)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:OpenSPDIF: Couldn't find suitable output"));
        return false;
    }

    if (!mRevertFormat)
    {
	AudioObjectPropertyAddress pa
	{
	    kAudioStreamPropertyPhysicalFormat,
	    kAudioObjectPropertyScopeGlobal,
	    kMythAudioObjectPropertyElementMain
	};

        // Retrieve the original format of this stream first
        // if not done so already
        UInt32 paramSize = sizeof(mFormatOrig);
        err = AudioObjectGetPropertyData(mStreamID, &pa, 0, nullptr,
					 &paramSize, &mFormatOrig);
        if (err != noErr)
        {
            LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:OpenSPDIF - could not retrieve the original streamformat: [%1]")
                 .arg(OSS_STATUS(err)));
        }
        else
        {
            mRevertFormat = true;
        }
    }

    mDigitalInUse = true;

    SetAutoHogMode(false);
    bool autoHog = GetAutoHogMode();
    if (!autoHog)
    {
        // Hog the device
        SetHogStatus(true);
        // Set mixable to false if we are allowed to
        SetMixingSupport(false);
    }

    mFormatNew = outputFormat;
    if (!AudioStreamChangeFormat(mStreamID, mFormatNew))
    {
        return false;
    }
    mBytesPerPacket = mFormatNew.mBytesPerPacket;

    // Add IOProc callback
    err = AudioDeviceCreateIOProcID(mDeviceID,
				    (AudioDeviceIOProc)RenderCallbackSPDIF,
				    (void *)this, &mIoProcID);
    if (err != noErr)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:OpenSPDIF: AudioDeviceCreateIOProcID failed: [%1]")
              .arg(OSS_STATUS(err)));
        return false;
    }
    mIoProc = true;

    // Start device
    err = AudioDeviceStart(mDeviceID, mIoProcID);
    if (err != noErr)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:OpenSPDIF: AudioDeviceStart failed: [%1]")
              .arg(OSS_STATUS(err)));
        return false;
    }
    mStarted = true;
    return true;
}

void CoreAudioData::CloseSPDIF()
{
    OSStatus  err;

    LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::CloseSPDIF: Entering [%1]").arg(mDigitalInUse));;
    if (!mDigitalInUse)
        return;

    // Stop device
    if (mStarted)
    {
        err = AudioDeviceStop(mDeviceID, mIoProcID);
        if (err != noErr)
            LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:CloseSPDIF: AudioDeviceStop failed: [%1]")
                  .arg(OSS_STATUS(err)));
        mStarted = false;
    }

    // Remove IOProc callback
    if (mIoProc)
    {
        err = AudioDeviceDestroyIOProcID(mDeviceID, mIoProcID);
        if (err != noErr)
            LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:CloseSPDIF: AudioDeviceDestroyIOProcID failed: [%1]")
                  .arg(OSS_STATUS(err)));
        mIoProc = false;
    }

    if (mRevertFormat)
    {
        AudioStreamChangeFormat(mStreamID, mFormatOrig);
        mRevertFormat = false;
    }

    SetHogStatus(false);
    if (mMixerRestore > -1) // We changed the mixer status
        SetMixingSupport((mMixerRestore != 0));
    AudioHardwareUnload();
    mMixerRestore = -1;
    mBytesPerPacket = -1;
    mStreamIndex = -1;
    mWasDigital = true;
}

int CoreAudioData::AudioStreamChangeFormat(AudioStreamID               s,
                                           AudioStreamBasicDescription format)
{
    LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::AudioStreamChangeFormat: %1 -> %2")
          .arg(s)
          .arg(StreamDescriptionToString(format)));

    AudioObjectPropertyAddress pa
    {
	kAudioStreamPropertyPhysicalFormat,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };
    OSStatus err = AudioObjectSetPropertyData(s, &pa, 0, nullptr,
					      sizeof(format), &format);
    if (err != noErr)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("CoreAudioData Error:AudioStreamChangeFormat couldn't set stream format: [%1]")
              .arg(OSS_STATUS(err)));
        return false;
    }
    return true;
}

bool CoreAudioData::FindAC3Stream()
{
    AudioStreamIDVec streams;


    // Get a list of all the streams on this device
    streams = StreamsList(mDeviceID);
    if (streams.empty())
        return false;

    for (auto stream : streams)
    {
        AudioStreamRangedVec formats = FormatsList(stream);
        if (formats.empty())
            continue;

        // Find a stream with a cac3 stream
        for (auto format : formats) {
            if (format.mFormat.mFormatID == 'IAC3' ||
                format.mFormat.mFormatID == kAudioFormat60958AC3)
            {
                LOG(VB_AUDIO, LOG_INFO, "CoreAudioData::FindAC3Stream: found digital format");
                return true;
            }
        }
    }

    return false;
}

/**
 * Reset any devices with an AC3 stream back to a Linear PCM
 * so that they can become a default output device
 */
void CoreAudioData::ResetAudioDevices()
{
    UInt32         size;
    AudioObjectPropertyAddress pa
    {
	kAudioHardwarePropertyDevices,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa,
						  0, nullptr, &size);
    if (err)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:GetPropertyDataSize: Unable to retrieve the property sizes. "
                     "Error [%1]")
             .arg(err));
	return;
    }

    std::vector<AudioDeviceID> devices = {};
    devices.resize(size / sizeof(AudioDeviceID));
    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa,
				     0, nullptr, &size, devices.data());
    if (err)
    {
        LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:GetPropertyData: Unable to retrieve the list of available devices. "
                     "Error [%1]")
             .arg(err));
	return;
    }

    for (const auto & dev : devices)
    {
        AudioStreamIDVec streams;

        streams = StreamsList(dev);
        if (streams.empty())
            continue;
        for (auto stream : streams)
            ResetStream(stream);
    }
}

void CoreAudioData::ResetStream(AudioStreamID s)
{
    AudioStreamBasicDescription  currentFormat;
    OSStatus                     err;
    UInt32                       paramSize;
    AudioObjectPropertyAddress pa
    {
	kAudioStreamPropertyPhysicalFormat,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };


    // Find the streams current physical format
    paramSize = sizeof(currentFormat);
    AudioObjectGetPropertyData(s, &pa, 0, nullptr,
			       &paramSize, &currentFormat);

    // If it's currently AC-3/SPDIF then reset it to some mixable format
    if (currentFormat.mFormatID == 'IAC3' ||
        currentFormat.mFormatID == kAudioFormat60958AC3)
    {
        AudioStreamRangedVec        formats    = FormatsList(s);


        if (formats.empty())
            return;

        for (auto format : formats) {
            if (format.mFormat.mFormatID == kAudioFormatLinearPCM)
            {
                LOG(VB_AUDIO, LOG_INFO, QString("CoreAudioData::ResetStream: Resetting stream %1 to %2").arg(s).arg(StreamDescriptionToString(format.mFormat)));
                err = AudioObjectSetPropertyData(s, &pa, 0, nullptr,
                                                 sizeof(format), &(format.mFormat));
                if (err != noErr)
                {
                    LOG(VB_GENERAL, LOG_WARNING, QString("CoreAudioData Warning:ResetStream: could not set physical format: [%1]")
                         .arg(OSS_STATUS(err)));
                    continue;
                }
                
                sleep(1);   // For the change to take effect
            }
        }
    }
}

QMap<QString, QString> *AudioOutputCA::GetDevices(const char */*type*/)
{
    auto *devs = new QMap<QString, QString>();

    // Obtain a list of all available audio devices
    UInt32 size = 0;

    AudioObjectPropertyAddress pa
    {
	kAudioHardwarePropertyDevices,
	kAudioObjectPropertyScopeGlobal,
	kMythAudioObjectPropertyElementMain
    };

    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa,
						  0, nullptr, &size);
    if (err)
    {
        LOG(VB_AUDIO, LOG_INFO, LOC + QString("GetPropertyDataSize: Unable to retrieve the property sizes. "
                     "Error [%1]")
             .arg(err));
	return devs;
    }

    UInt32 deviceCount = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devices = {};
    devices.resize(deviceCount);
    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa,
                                    0, nullptr, &size, devices.data());
    if (err)
    {
        LOG(VB_AUDIO, LOG_INFO, LOC + QString("AudioOutputCA::GetDevices: Unable to retrieve the list of "
                        "available devices. Error [%1]")
                .arg(err));
    }
    else
    {
        LOG(VB_AUDIO, LOG_INFO, LOC + QString("GetDevices: Number of devices: %1").arg(deviceCount));

        for (const auto & dev : devices)
        {
            CoreAudioData device(nullptr, dev);
            if (device.GetTotalOutputChannels() == 0)
                continue;
            QString *name = device.GetName();
            if (name)
            {
                devs->insert(*name, QString());
                delete name;
            }
        }
    }
    return devs;
}
