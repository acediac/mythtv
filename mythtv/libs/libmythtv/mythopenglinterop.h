#ifndef MYTHOPENGLINTEROP_H
#define MYTHOPENGLINTEROP_H

// Qt
#include <QObject>

// MythTV
#include "mythrender_opengl.h"
#include "referencecounter.h"
#include "videoouttypes.h"
#include "mythcodecid.h"
#include "mythframe.h"
#include "mythvideotexture.h"

// Std
#include "vector"
using std::vector;

class VideoColourSpace;

#define DUMMY_INTEROP_ID 1

class MythOpenGLInterop : public QObject, public ReferenceCounter
{
    Q_OBJECT

  public:

    enum Type
    {
        Unsupported  = 0,
        VAAPIGLXCOPY = 1,
        VAAPIGLXPIX  = 2,
        VAAPIEGLDRM  = 3,
        VTBOPENGL    = 4,
        VTBSURFACE   = 5,
        MEDIACODEC   = 6,
        VDPAU        = 7,
        NVDEC
    };

    static QStringList GetAllowedRenderers   (MythCodecID CodecId);
    static Type        GetInteropType        (MythCodecID CodecId);
    static vector<MythVideoTexture*> Retrieve(MythRenderOpenGL *Context,
                                              VideoColourSpace *ColourSpace,
                                              VideoFrame *Frame,
                                              FrameScanType Scan);
    static QString     TypeToString          (Type InteropType);

    virtual ~MythOpenGLInterop();
    virtual vector<MythVideoTexture*> Acquire(MythRenderOpenGL *Context,
                                              VideoColourSpace *ColourSpace,
                                              VideoFrame *Frame, FrameScanType Scan) = 0;
    Type GetType(void);

  protected:
    explicit MythOpenGLInterop(MythRenderOpenGL *Context, Type InteropType);

  protected:
    MythRenderOpenGL   *m_context;
    Type                m_type;
    QHash<unsigned long long, vector<MythVideoTexture*> > m_openglTextures;
    QSize               m_openglTextureSize;
};

#endif // MYTHOPENGLINTEROP_H
