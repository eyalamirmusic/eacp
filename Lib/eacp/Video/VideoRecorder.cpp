#include "VideoRecorder.h"

#include "AudioRing.h"
#include "Encoder.h"
#include "ScreenCapture.h"

#include <eacp/Graphics/Graphics.h>
#include <eacp/Graphics/Helpers/DisplayLink.h>

#include <atomic>
#include <chrono>
#include <thread>

// The portable half of the recorder: tier dispatch, the two DisplayLink-driven
// off-screen tiers (Snapshot and GpuDirect) and the audio drain, all of which
// are identical on every platform. Everything platform-specific sits behind the
// Encoder interface (AVFoundation / Media Foundation) and the ScreenCapture
// interface (ScreenCaptureKit / Windows.Graphics.Capture).

namespace eacp::Video
{
namespace
{
int roundDownToEven(int value)
{
    return value & ~1;
}

double nowSeconds()
{
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(since).count();
}

// A second of backlog: the drain empties the ring every few milliseconds, so
// this only has to survive a stalled encoder, not carry the recording.
int ringCapacityFor(const AudioSpec& spec)
{
    return spec.sampleRate;
}

// Long enough that the encoder is handed useful work per call, short enough
// that the drain stays responsive to stop().
int drainBlockFrames(const AudioSpec& spec)
{
    return spec.sampleRate / 50;
}
} // namespace

struct VideoRecorder::Native
{
    // Every tier timestamps against the moment start() ran, so the audio the
    // app pushes and the frames the platform hands back share one origin.
    double elapsed() const { return nowSeconds() - startSeconds; }

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

        encoder->appendNativeContent(*view, scale, elapsed());
    }

    void captureFrame(Threads::FrameTime frameTime)
    {
        if (!recording || !paceAllows(frameTime.time))
            return;

        auto image = view->renderToImage(scale);
        if (!image.isValid() || image.width() < width || image.height() < height)
            return;

        encoder->appendImage(image, elapsed());
    }

    EncoderSpec specFor(const RecordingOptions& options) const
    {
        auto spec = EncoderSpec {};
        spec.video.width = width;
        spec.video.height = height;
        spec.video.fps = options.fps > 0 ? options.fps : 60;
        spec.video.bitrate =
            options.bitrate > 0 ? options.bitrate : width * height * 8;
        spec.audio = options.audio;
        return spec;
    }

    // The size every off-screen tier records at: renderToImage resolves the
    // backing scale itself when options.scale is 0.
    bool probeSize(Graphics::View& viewToUse, const RecordingOptions& options)
    {
        view = &viewToUse;
        scale = options.scale;

        auto probe = viewToUse.renderToImage(options.scale);
        width = roundDownToEven(probe.width());
        height = roundDownToEven(probe.height());

        return width > 0 && height > 0;
    }

    void startDisplayLink(const RecordingOptions& options,
                          const Threads::DisplayLink::FrameCallback& capture)
    {
        frameInterval = options.fps > 0 ? 1.0 / options.fps : 0.0;
        nextCapture = 0.0;
        recording = true;
        link = makeOwned<Threads::DisplayLink>(capture);
    }

    bool startSnapshot(Graphics::View& viewToUse,
                       const FilePath& path,
                       const RecordingOptions& options)
    {
        if (!probeSize(viewToUse, options)
            || !encoder->begin(path, specFor(options)))
            return false;

        auto* native = this;
        startDisplayLink(options,
                         [native](Threads::FrameTime time)
                         { native->captureFrame(time); });

        return true;
    }

    bool startGpuDirect(Graphics::View& viewToUse,
                        const FilePath& path,
                        const RecordingOptions& options)
    {
        if (!probeSize(viewToUse, options))
            return false;

        // Confirm the view actually renders native GPU content this encoder can
        // capture before committing; a plain 2D/WebView view has none, and no
        // encoder supports it on some platforms, so GpuDirect does not apply.
        if (!encoder->canCaptureNativeContent(viewToUse, scale, width, height))
            return false;

        if (!encoder->begin(path, specFor(options)))
            return false;

        auto* native = this;
        startDisplayLink(options,
                         [native](Threads::FrameTime time)
                         { native->captureGpuDirectFrame(time); });

        return true;
    }

    bool startScreen(Graphics::View& viewToUse,
                     const FilePath& path,
                     const RecordingOptions& options)
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

    void startAudio(const AudioSpec& spec)
    {
        audioSpec = spec;
        audioAnchor.store(-1.0, std::memory_order_relaxed);
        audioRing.prepare(spec.numChannels, ringCapacityFor(spec));

        audioRunning = true;
        audioThread = std::thread(
            [this]
            {
                while (audioRunning.load(std::memory_order_relaxed))
                {
                    appendReadyAudio();
                    Time::sleepMS(5);
                }
            });
    }

    void stopAudio()
    {
        if (!audioThread.joinable())
            return;

        audioRunning = false;
        audioThread.join();

        appendReadyAudio();
        audioSpec.reset();
    }

    // Nothing is drained until the encoder has an audio track open: the screen
    // tier opens the file asynchronously, and anything read out before then
    // would go nowhere. It waits in the ring instead, keeping the position on
    // the timeline it was pushed at.
    void appendReadyAudio()
    {
        auto anchor = audioAnchor.load(std::memory_order_acquire);

        if (anchor < 0.0 || !audioSpec || !encoder->acceptsAudio())
            return;

        for (;;)
        {
            auto frameIndex = audioRing.framesRead();
            auto block = audioRing.read(drainBlockFrames(*audioSpec));

            if (!block.isValid())
                return;

            encoder->appendAudio(block,
                                 audioTimeFor(frameIndex, anchor, *audioSpec));
        }
    }

    CaptureMode mode = CaptureMode::Snapshot;
    OwningPointer<Encoder> encoder = makeEncoder();
    OwningPointer<ScreenCapture> screen;
    std::atomic<bool> recording {false};

    // Between stop() and the file actually being closed. The screen tier
    // finalizes asynchronously, so `recording` going false is NOT the encoder
    // and the capture becoming reusable -- and starting over them replaces the
    // capture object while its own stop callback still holds it.
    std::atomic<bool> finalizing {false};
    double startSeconds = 0.0;

    // Off-screen tiers (Snapshot, GpuDirect) share this DisplayLink-driven state.
    Graphics::View* view = nullptr;
    float scale = 0.0f;
    int width = 0;
    int height = 0;
    double frameInterval = 0.0;
    double nextCapture = 0.0;
    OwningPointer<Threads::DisplayLink> link;

    // Written by the audio thread, read by the drain. The anchor is where the
    // app's first block landed on the recording's timeline; everything after it
    // follows by sample count.
    std::optional<AudioSpec> audioSpec;
    AudioRing audioRing;
    std::atomic<double> audioAnchor {-1.0};
    std::atomic<bool> audioRunning {false};
    std::thread audioThread;
};

VideoRecorder::VideoRecorder() = default;

VideoRecorder::~VideoRecorder()
{
    impl->recording = false;
    impl->stopAudio();
}

bool VideoRecorder::isRecording() const
{
    return impl->recording;
}

int VideoRecorder::droppedAudioFrames() const
{
    return impl->audioRing.droppedFrames();
}

bool VideoRecorder::start(Graphics::View& view,
                          const FilePath& path,
                          const RecordingOptions& options)
{
    if (impl->recording || impl->finalizing)
        return false;

    impl->mode = options.mode;
    impl->startSeconds = nowSeconds();

    auto started = [&]
    {
        if (options.mode == CaptureMode::Screen)
            return impl->startScreen(view, path, options);

        if (options.mode == CaptureMode::GpuDirect)
            return impl->startGpuDirect(view, path, options);

        return impl->startSnapshot(view, path, options);
    }();

    if (started && options.audio)
        impl->startAudio(*options.audio);

    return started;
}

void VideoRecorder::pushAudio(const AudioBuffer& buffer) noexcept
{
    if (!impl->audioRunning.load(std::memory_order_relaxed))
        return;

    if (impl->audioAnchor.load(std::memory_order_relaxed) < 0.0)
        impl->audioAnchor.store(impl->elapsed(), std::memory_order_release);

    impl->audioRing.write(buffer);
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
    impl->finalizing = true;

    // Before the encoder is finalized, and before the screen tier's own stop
    // reaches for it: the drain is the one other thread still writing samples.
    impl->stopAudio();

    auto finished = [this]
    {
        if (impl->mode == CaptureMode::Screen)
            return impl->screen->stop();

        impl->link = nullptr; // stop the off-screen display link
        return impl->encoder->finish();
    }();

    auto* native = impl.get();
    finished.then([native] { native->finalizing = false; });

    return finished;
}

} // namespace eacp::Video
