#pragma once

#import <AVFoundation/AVFoundation.h>

#include "Encoder.h"

#include <eacp/Core/ObjC/ObjC.h>

// Apple-only header (ObjC++), shared by Encoder-Apple.mm and ScreenCapture-Apple.
// The Screen tier appends IOSurface-backed CVPixelBuffers straight from
// ScreenCaptureKit, so it needs the concrete AppleEncoder's pool()/append(), not
// just the portable Encoder interface.

namespace eacp::Video
{

// AVAssetWriter H.264 encoder fed BGRA CVPixelBuffers with PTS. The writer
// session starts at the first appended frame's timestamp, so both a zero-based
// snapshot clock and ScreenCaptureKit's host clock work unchanged.
struct AppleEncoder final : Encoder
{
    bool begin(const FilePath& path, int w, int h, int bitrate, int fps) override;
    void appendImage(const Graphics::Image& image, double ptsSeconds) override;
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
    bool sessionStarted = false;
    int width = 0;
    int height = 0;
};

} // namespace eacp::Video
