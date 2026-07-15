#include "VideoRecorder.h"

#include "Encoder.h"
#include "ScreenCapture.h"

#include <eacp/Graphics/Graphics.h>
#include <eacp/Graphics/Helpers/DisplayLink.h>

// The portable half of the recorder: tier dispatch and the two DisplayLink-driven
// off-screen tiers (Snapshot and GpuDirect), which are identical on every
// platform. Everything platform-specific sits behind the Encoder interface
// (AVFoundation / Media Foundation) and the ScreenCapture interface
// (ScreenCaptureKit / Windows.Graphics.Capture).

namespace eacp::Video
{
namespace
{
int roundDownToEven(int value)
{
    return value & ~1;
}
} // namespace

struct VideoRecorder::Native
{
    // Holds the target frame rate against a faster display: returns true only
    // when the next scheduled slot is due, advancing on an ideal grid so it does
    // not drift. Resyncs if a slow frame put us behind.
    bool paceAllows(double time)
    {
        if (frameInterval <= 0.0)
            return true;

        if (time < nextCapture)
            return false;

        nextCapture += frameInterval;
        if (nextCapture <= time)
            nextCapture = time + frameInterval;

        return true;
    }

    // GpuDirect: hand the encoder the view so it can render the GPU content
    // straight into a shared GPU frame and append it -- the pixels never touch
    // the CPU. A no-op on encoders without native support.
    void captureGpuDirectFrame(Threads::FrameTime frameTime)
    {
        if (!recording || !paceAllows(frameTime.time))
            return;

        encoder->appendNativeContent(*view, scale, frameTime.time);
    }

    void captureFrame(Threads::FrameTime frameTime)
    {
        if (!recording || !paceAllows(frameTime.time))
            return;

        auto image = view->renderToImage(scale);
        if (!image.isValid() || image.width() < width || image.height() < height)
            return;

        encoder->appendImage(image, frameTime.time);
    }

    bool startSnapshot(Graphics::View& viewToUse,
                       const FilePath& path,
                       const VideoOptions& options)
    {
        view = &viewToUse;
        scale = options.scale;

        // Probe one frame to size the video; renderToImage resolves the backing
        // scale itself when options.scale is 0.
        auto probe = viewToUse.renderToImage(options.scale);
        width = roundDownToEven(probe.width());
        height = roundDownToEven(probe.height());

        if (width <= 0 || height <= 0)
            return false;

        auto fps = options.fps > 0 ? options.fps : 60;
        auto bitrate = options.bitrate > 0 ? options.bitrate : width * height * 8;
        if (!encoder->begin(path, width, height, bitrate, fps))
            return false;

        frameInterval = options.fps > 0 ? 1.0 / options.fps : 0.0;
        nextCapture = 0.0;
        recording = true;

        auto* native = this;
        link = makeOwned<Threads::DisplayLink>([native](Threads::FrameTime time)
                                               { native->captureFrame(time); });

        return true;
    }

    bool startGpuDirect(Graphics::View& viewToUse,
                        const FilePath& path,
                        const VideoOptions& options)
    {
        view = &viewToUse;
        scale = options.scale;

        // Probe for size (renderToImage resolves the backing scale when scale is
        // 0). GPU content itself is captured zero-copy below.
        auto probe = viewToUse.renderToImage(options.scale);
        width = roundDownToEven(probe.width());
        height = roundDownToEven(probe.height());
        if (width <= 0 || height <= 0)
            return false;

        // Confirm the view actually renders native GPU content this encoder can
        // capture before committing; a plain 2D/WebView view has none, and no
        // encoder supports it on some platforms, so GpuDirect does not apply.
        if (!encoder->canCaptureNativeContent(viewToUse, scale, width, height))
            return false;

        auto fps = options.fps > 0 ? options.fps : 60;
        auto bitrate = options.bitrate > 0 ? options.bitrate : width * height * 8;
        if (!encoder->begin(path, width, height, bitrate, fps))
            return false;

        frameInterval = options.fps > 0 ? 1.0 / options.fps : 0.0;
        nextCapture = 0.0;
        recording = true;

        auto* native = this;
        link = makeOwned<Threads::DisplayLink>(
            [native](Threads::FrameTime time)
            { native->captureGpuDirectFrame(time); });

        return true;
    }

    bool startScreen(Graphics::View& viewToUse,
                     const FilePath& path,
                     const VideoOptions& options)
    {
        screen = makeScreenCapture();
        recording = true;

        if (!screen->start(viewToUse, path, options, *encoder))
        {
            recording = false;
            return false;
        }

        return true;
    }

    CaptureMode mode = CaptureMode::Snapshot;
    OwningPointer<Encoder> encoder = makeEncoder();
    OwningPointer<ScreenCapture> screen;
    bool recording = false;

    // Off-screen tiers (Snapshot, GpuDirect) share this DisplayLink-driven state.
    Graphics::View* view = nullptr;
    float scale = 0.0f;
    int width = 0;
    int height = 0;
    double frameInterval = 0.0;
    double nextCapture = 0.0;
    OwningPointer<Threads::DisplayLink> link;
};

VideoRecorder::VideoRecorder() = default;
VideoRecorder::~VideoRecorder() = default;

bool VideoRecorder::isRecording() const
{
    return impl->recording;
}

bool VideoRecorder::start(Graphics::View& view,
                          const FilePath& path,
                          const VideoOptions& options)
{
    if (impl->recording)
        return false;

    impl->mode = options.mode;

    if (options.mode == CaptureMode::Screen)
        return impl->startScreen(view, path, options);

    if (options.mode == CaptureMode::GpuDirect)
        return impl->startGpuDirect(view, path, options);

    return impl->startSnapshot(view, path, options);
}

Threads::Async<void> VideoRecorder::stop()
{
    if (!impl->recording)
    {
        auto promise = Threads::AsyncPromise<void> {};
        promise.resolve();
        return promise.get();
    }

    impl->recording = false;

    if (impl->mode == CaptureMode::Screen)
        return impl->screen->stop();

    impl->link = nullptr; // stop the off-screen display link
    return impl->encoder->finish();
}

} // namespace eacp::Video
