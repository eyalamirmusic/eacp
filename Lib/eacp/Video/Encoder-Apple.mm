#import <AppKit/AppKit.h>

#include "Encoder-Apple.h"

#include <eacp/Core/Utils/Strings.h>
#include <eacp/Graphics/Graphics.h>

#include <algorithm>
#include <cstdint>

namespace eacp::Video
{
namespace
{
NSString* fileTypeForPath(const FilePath& path)
{
    auto extension = Strings::toLower(path.extension());

    if (extension == ".mp4" || extension == "mp4")
        return AVFileTypeMPEG4;

    return AVFileTypeQuickTimeMovie;
}

// A standalone IOSurface-backed, Metal-compatible BGRA buffer (for the GpuDirect
// support probe, before the encoder's pool exists). Caller releases.
CVPixelBufferRef makeMetalPixelBuffer(int width, int height)
{
    NSDictionary* attributes = @{
        (id) kCVPixelBufferMetalCompatibilityKey : @YES,
        (id) kCVPixelBufferIOSurfacePropertiesKey : @ {}
    };

    CVPixelBufferRef buffer = nullptr;
    CVPixelBufferCreate(nullptr,
                        (size_t) width,
                        (size_t) height,
                        kCVPixelFormatType_32BGRA,
                        (CFDictionaryRef) attributes,
                        &buffer);
    return buffer;
}

AVAssetWriterInput* makeVideoInput(const VideoSpec& spec)
{
    NSDictionary* compression = @{AVVideoAverageBitRateKey : @(spec.bitrate)};
    NSDictionary* settings = @{
        AVVideoCodecKey : AVVideoCodecTypeH264,
        AVVideoWidthKey : @(spec.width),
        AVVideoHeightKey : @(spec.height),
        AVVideoCompressionPropertiesKey : compression
    };

    auto* in = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                              outputSettings:settings];
    in.expectsMediaDataInRealTime = YES;
    return in;
}

AVAssetWriterInput* makeAudioInput(const AudioSpec& spec)
{
    NSDictionary* settings = @{
        AVFormatIDKey : @(kAudioFormatMPEG4AAC),
        AVSampleRateKey : @(spec.sampleRate),
        AVNumberOfChannelsKey : @(spec.numChannels),
        AVEncoderBitRateKey : @(audioBitrateFor(spec))
    };

    auto* in = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio
                                              outputSettings:settings];
    in.expectsMediaDataInRealTime = YES;
    return in;
}

// What appendAudio hands the writer: interleaved 32-bit float, which the input
// transcodes to AAC on its way into the file.
CMFormatDescriptionRef makeAudioFormat(const AudioSpec& spec)
{
    auto bytesPerFrame = (UInt32) (sizeof(float) * (std::size_t) spec.numChannels);

    auto description = AudioStreamBasicDescription {};
    description.mSampleRate = spec.sampleRate;
    description.mFormatID = kAudioFormatLinearPCM;
    description.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    description.mBytesPerPacket = bytesPerFrame;
    description.mFramesPerPacket = 1;
    description.mBytesPerFrame = bytesPerFrame;
    description.mChannelsPerFrame = (UInt32) spec.numChannels;
    description.mBitsPerChannel = 32;

    CMFormatDescriptionRef format = nullptr;
    if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault,
                                       &description,
                                       0,
                                       nullptr,
                                       0,
                                       nullptr,
                                       nullptr,
                                       &format)
        != noErr)
        return nullptr;

    return format;
}

void waitForInput(AVAssetWriterInput* in, Time::MS timeout)
{
    // AVAssetWriterInput has no blocking form, so this polls. The wait is short
    // in practice — the encoder drains in well under a millisecond — and the
    // deadline keeps a stalled writer from hanging the caller outright.
    auto deadline = Time::Deadline {timeout};

    while (![in isReadyForMoreMediaData] && !deadline.expired())
        Time::sleepMS(1);
}
} // namespace

bool AppleEncoder::begin(const FilePath& path, const EncoderSpec& spec)
{
    width = spec.video.width;
    height = spec.video.height;

    auto* url = [NSURL fileURLWithPath:@(path.c_str())];
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];

    NSError* error = nil;
    auto* writerObject = [[AVAssetWriter alloc] initWithURL:url
                                                   fileType:fileTypeForPath(path)
                                                      error:&error];
    if (writerObject == nil)
        return false;

    auto* in = makeVideoInput(spec.video);

    // IOSurface + Metal compatibility so the GpuDirect tier can render straight
    // into pool buffers; harmless for the CPU-filled snapshot tier.
    NSDictionary* pixelAttributes = @{
        (id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (id) kCVPixelBufferWidthKey : @(width),
        (id) kCVPixelBufferHeightKey : @(height),
        (id) kCVPixelBufferMetalCompatibilityKey : @YES,
        (id) kCVPixelBufferIOSurfacePropertiesKey : @ {}
    };

    auto* ad = [[AVAssetWriterInputPixelBufferAdaptor alloc]
        initWithAssetWriterInput:in
        sourcePixelBufferAttributes:pixelAttributes];

    auto* audioIn = spec.audio ? makeAudioInput(*spec.audio) : nil;
    auto format = CFRef<CMFormatDescriptionRef> {
        spec.audio ? makeAudioFormat(*spec.audio) : nullptr};

    auto ok = [writerObject canAddInput:in];

    if (ok)
        [writerObject addInput:in];

    // Every track has to be declared before the writer starts: an input added
    // afterwards is refused.
    if (ok && audioIn != nil)
    {
        ok = format && [writerObject canAddInput:audioIn];

        if (ok)
            [writerObject addInput:audioIn];
    }

    if (ok)
        ok = [writerObject startWriting];

    if (!ok)
    {
        [writerObject release];
        [in release];
        [ad release];
        [audioIn release];
        return false;
    }

    writer = writerObject;
    input = in;
    adaptor = ad;
    audioInput = audioIn;
    audioFormat = std::move(format);
    audioSpec = spec.audio.value_or(AudioSpec {});

    // The encoder outlives one recording -- a second start() reuses it -- and
    // the new writer has a session of its own to open.
    auto lock = std::lock_guard {sessionMutex};
    sessionStarted = false;

    return true;
}

CVPixelBufferPoolRef AppleEncoder::pool() const
{
    return adaptor.get().pixelBufferPool;
}

void AppleEncoder::startSessionIfNeeded(CMTime pts)
{
    auto lock = std::lock_guard {sessionMutex};

    if (sessionStarted)
        return;

    [writer.get() startSessionAtSourceTime:pts];
    sessionStarted = true;
}

void AppleEncoder::append(CVPixelBufferRef buffer, CMTime pts)
{
    startSessionIfNeeded(pts);

    if ([input.get() isReadyForMoreMediaData])
        [adaptor.get() appendPixelBuffer:buffer withPresentationTime:pts];
}

void AppleEncoder::waitUntilReady(Time::MS timeout)
{
    if (input)
        waitForInput(input.get(), timeout);
}

void AppleEncoder::appendImage(const Graphics::Image& image, double ptsSeconds)
{
    auto* bufferPool = pool();
    if (bufferPool == nullptr)
        return;

    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, bufferPool, &buffer)
        != kCVReturnSuccess)
        return;

    CVPixelBufferLockBaseAddress(buffer, 0);

    auto* dst = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    auto dstStride = CVPixelBufferGetBytesPerRow(buffer);
    compositeOverBlackBGRA(image, dst, width, height, dstStride);

    CVPixelBufferUnlockBaseAddress(buffer, 0);

    append(buffer, CMTimeMakeWithSeconds(ptsSeconds, 600));
    CVPixelBufferRelease(buffer);
}

void AppleEncoder::appendAudio(const AudioBuffer& buffer, double ptsSeconds)
{
    if (!audioInput || !audioFormat || !buffer.isValid())
        return;

    auto channels = audioSpec.numChannels;
    auto frames = buffer.numFrames;
    interleaved.resize(frames * channels);

    for (auto channel = 0; channel < channels; ++channel)
    {
        auto source = buffer.channel(std::min(channel, buffer.numChannels - 1));
        auto* destination = interleaved.data() + channel;

        for (auto frame = 0; frame < frames; ++frame)
            destination[frame * channels] = source[frame];
    }

    auto byteCount = (std::size_t) interleaved.size() * sizeof(float);

    CMBlockBufferRef rawBlock = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,
                                           nullptr,
                                           byteCount,
                                           kCFAllocatorDefault,
                                           nullptr,
                                           0,
                                           byteCount,
                                           kCMBlockBufferAssureMemoryNowFlag,
                                           &rawBlock)
        != kCMBlockBufferNoErr)
        return;

    auto block = CFRef<CMBlockBufferRef> {rawBlock};

    if (CMBlockBufferReplaceDataBytes(interleaved.data(), block.get(), 0, byteCount)
        != kCMBlockBufferNoErr)
        return;

    auto pts = CMTimeMakeWithSeconds(ptsSeconds, audioSpec.sampleRate);

    CMSampleBufferRef rawSample = nullptr;
    if (CMAudioSampleBufferCreateReadyWithPacketDescriptions(kCFAllocatorDefault,
                                                             block.get(),
                                                             audioFormat.get(),
                                                             frames,
                                                             pts,
                                                             nullptr,
                                                             &rawSample)
        != noErr)
        return;

    auto sample = CFRef<CMSampleBufferRef> {rawSample};

    startSessionIfNeeded(pts);

    // A dropped block is a hole in the sound rather than a missing frame, so
    // this waits for the input instead of discarding it.
    waitForInput(audioInput.get(), Time::MS {200});

    if ([audioInput.get() isReadyForMoreMediaData])
        [audioInput.get() appendSampleBuffer:sample.get()];
}

bool AppleEncoder::canCaptureNativeContent(Graphics::View& view,
                                           float scale,
                                           int probeWidth,
                                           int probeHeight)
{
    auto probeBuffer = makeMetalPixelBuffer(probeWidth, probeHeight);
    auto supported = probeBuffer != nullptr
                     && view.renderNativeContentToTarget(probeBuffer, scale);
    if (probeBuffer != nullptr)
        CVPixelBufferRelease(probeBuffer);

    return supported;
}

bool AppleEncoder::appendNativeContent(Graphics::View& view, float scale, double pts)
{
    auto* bufferPool = pool();
    if (bufferPool == nullptr)
        return false;

    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, bufferPool, &buffer)
        != kCVReturnSuccess)
        return false;

    auto captured = view.renderNativeContentToTarget(buffer, scale);
    if (captured)
        append(buffer, CMTimeMakeWithSeconds(pts, 600));

    CVPixelBufferRelease(buffer);
    return captured;
}

Threads::Async<void> AppleEncoder::finish()
{
    auto promise = Threads::AsyncPromise<void> {};
    auto result = promise.get();

    if (!writer)
    {
        promise.resolve();
        return result;
    }

    [input.get() markAsFinished];

    if (audioInput)
        [audioInput.get() markAsFinished];

    [writer.get() finishWritingWithCompletionHandler:^{
        Threads::callAsync([promise] { promise.resolve(); });
    }];

    return result;
}

OwningPointer<Encoder> makeEncoder()
{
    return makeOwned<AppleEncoder>();
}

} // namespace eacp::Video
