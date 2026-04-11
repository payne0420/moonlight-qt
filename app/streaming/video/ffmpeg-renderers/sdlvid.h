#pragma once

#include "renderer.h"
#include "swframemapper.h"

#include <QVector>

#ifdef HAVE_CUDA
#include "cuda.h"
#endif

extern "C" {
#include <libswscale/swscale.h>
}

class SdlRenderer : public IFFmpegRenderer {
public:
    SdlRenderer();
    virtual ~SdlRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void prepareToRender() override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual bool isRenderThreadSupported() override;
    virtual bool isPixelFormatSupported(int videoFormat, enum AVPixelFormat pixelFormat) override;
    virtual bool testRenderFrame(AVFrame* frame) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) override;

    // Multi-monitor: set extra windows that each display a horizontal slice of the frame
    void setMultiMonitorWindows(const QVector<SDL_Window*>& windows,
                                int perMonitorWidth, int perMonitorHeight);

private:
    void renderOverlay(Overlay::OverlayType type);
    void renderExtraMonitors(AVFrame* frame);

    static void ffNoopFree(void *opaque, uint8_t *data);

    int m_VideoFormat;
    SDL_Renderer* m_Renderer;
    SDL_Texture* m_Texture;
    SDL_Texture* m_OverlayTextures[Overlay::OverlayMax];
    SDL_Rect m_OverlayRects[Overlay::OverlayMax];

    // Used for CPU conversion of YUV to RGB if needed
    bool m_NeedsYuvToRgbConversion;
    SwsContext* m_SwsContext;
    AVFrame* m_RgbFrame;

    SwFrameMapper m_SwFrameMapper;

    // Multi-monitor extra windows (index 0 = second monitor, etc.)
    struct ExtraMonitor {
        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        SDL_Texture* texture = nullptr;
    };
    QVector<ExtraMonitor> m_ExtraMonitors;
    int m_PerMonitorWidth = 0;
    int m_PerMonitorHeight = 0;
    int m_MonitorCount = 1;  // total monitors including primary

#ifdef HAVE_CUDA
    CUDAGLInteropHelper* m_CudaGLHelper;
#endif
};

