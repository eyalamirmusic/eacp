#pragma once

#import <AVFoundation/AVFoundation.h>

#include "Encoder.h"

#include <eacp/Core/ObjC/CFRef.h>
#include <eacp/Core/ObjC/ObjC.h>

#include <mutex>

// Apple-only header (ObjC++), shared by Encoder-Apple.mm and ScreenCapture-Apple.
// The Screen tier appends IOSurface-backed CVPixelBuffers straight from
// ScreenCaptureKit, so it needs the concrete AppleEncoder's pool()/append(), not
// just the portable Encoder interface.

namespace eacp::Video
{

// AVAssetWriter H.264 encoder fed BGRA CVPixelBuffers with PTS, plus an AAC
// track when the spec asks for one. The writer session starts at the first
// sample of either track, so a zero-based recorder timeline and a rebased
// ScreenCaptureKit clock both work unchanged.
struct AppleEncoder final : Encoder
{
    bool begin(const FilePath& path, const EncoderSpec& spec) override;
    void appendImage(const Graphics::Image& image, double ptsSeconds) override;
    void appendAudio(const AudioBuffer& buffer, double ptsSeconds) override;
    bool acceptsAudio() const override { return audioInput && audioFormat; }
    void waitUntilReady(Time::MS timeout) override;
    bool canCaptureNativeContent(Graphics::View& view,
                                 float scale,
                                 int probeWidth,
                                 int probeHeight) override;
    bool appendNativeContent(Graphics::View& view, float scale, double pts) override;
    Threads::Async<void> finish() override;

    bool valid() const { return writer && input && adaptor; }

    // Apple-only, used by the Screen tier: the pool the adaptor sources buffers
    // from, and a raw CVPixelBuffer append with an explicit PTS.
    CVPixelBufferPoolRef pool() const;
    void append(CVPixelBufferRef buffer, CMTime pts);

    ObjC::Ptr<AVAssetWriter> writer;
    ObjC::Ptr<AVAssetWriterInput> input;
    ObjC::Ptr<AVAssetWriterInputPixelBufferAdaptor> adaptor;
    ObjC::Ptr<AVAssetWriterInput> audioInput;
    CFRef<CMFormatDescriptionRef> audioFormat;
    AudioSpec audioSpec;
    Vector<float> interleaved;
    int width = 0;
    int height = 0;

private:
    // Frames and samples arrive on different threads, and only the first of
    // them may open the session.
    void startSessionIfNeeded(CMTime pts);

    std::mutex sessionMutex;
    bool sessionStarted = false;
};

} // namespace eacp::Video
