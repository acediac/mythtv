#ifndef MYTHVDPAUHELPER_H
#define MYTHVDPAUHELPER_H

// Qt
#include <QSize>
#include <QMutex>

// FFmpeg
extern "C" {
#include "libavutil/hwcontext_vdpau.h"
#include "vdpau/vdpau_x11.h"
#include "libavcodec/avcodec.h"
}

class MythXDisplay;
class VideoColourSpace;

class MythVDPAUHelper
{
  public:
    static bool   HaveVDPAU(void);
    static bool   HaveMPEG4Decode(void);
    static bool   CheckH264Decode(AVCodecContext *Context);
    static bool   CheckHEVCDecode(AVCodecContext *Context);

    explicit MythVDPAUHelper(AVVDPAUDeviceContext *Context);
    ~MythVDPAUHelper(void);

    bool             IsValid(void);
    bool             IsFeatureAvailable(uint Feature);
    VdpOutputSurface CreateOutputSurface(QSize Size);
    VdpVideoMixer    CreateMixer(QSize Size, VdpChromaType ChromaType = VDP_CHROMA_TYPE_420);
    void             MixerRender(VdpVideoMixer Mixer, VdpVideoSurface Source, VdpOutputSurface Dest);
    void             SetCSCMatrix(VdpVideoMixer Mixer, VideoColourSpace *ColourSpace);
    void             DeleteOutputSurface(VdpOutputSurface Surface);
    void             DeleteMixer(VdpVideoMixer Mixer);
    QSize            GetSurfaceParameters(VdpVideoSurface Surface, VdpChromaType &Chroma);

  protected:
    MythVDPAUHelper(void);

    bool   CheckMPEG4(void);
    bool   H264DecodeCheck(VdpDecoderProfile Profile, AVCodecContext *Context);
    bool   H264ProfileCheck(VdpDecoderProfile Profile, AVCodecContext *Context);
    bool   HEVCProfileCheck(AVCodecContext *Context);

    static QMutex gVDPAULock;
    static bool   gVDPAUAvailable;
    static bool   gVDPAUMPEG4Available;

  private:
    bool   InitProcs(void);

  private:
    bool                              m_valid                            { false   };
    bool                              m_createdDevice                    { false   };
    VdpDevice                         m_device                           { 0       };
    MythXDisplay                     *m_display                          { nullptr };

    VdpGetProcAddress                *m_vdpGetProcAddress                { nullptr };
    VdpGetErrorString                *m_vdpGetErrorString                { nullptr };
    VdpGetInformationString          *m_vdpGetInformationString          { nullptr };
    VdpDeviceDestroy                 *m_vdpDeviceDestroy                 { nullptr };
    VdpDecoderQueryCapabilities      *m_vdpDecoderQueryCapabilities      { nullptr };
    VdpDecoderCreate                 *m_vdpDecoderCreate                 { nullptr };
    VdpDecoderDestroy                *m_vdpDecoderDestroy                { nullptr };
    VdpVideoMixerCreate              *m_vdpVideoMixerCreate              { nullptr };
    VdpVideoMixerDestroy             *m_vdpVideoMixerDestroy             { nullptr };
    VdpVideoMixerRender              *m_vdpVideoMixerRender              { nullptr };
    VdpVideoMixerSetAttributeValues  *m_vdpVideoMixerSetAttributeValues  { nullptr };
    VdpVideoMixerSetFeatureEnables   *m_vdpVideoMixerSetFeatureEnables   { nullptr };
    VdpVideoMixerQueryFeatureSupport *m_vdpVideoMixerQueryFeatureSupport { nullptr };
    VdpOutputSurfaceCreate           *m_vdpOutputSurfaceCreate           { nullptr };
    VdpOutputSurfaceDestroy          *m_vdpOutputSurfaceDestroy          { nullptr };
    VdpVideoSurfaceGetParameters     *m_vdpVideoSurfaceGetParameters     { nullptr };
};

#endif // MYTHVDPAUHELPER_H
