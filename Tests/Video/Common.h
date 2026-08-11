#pragma once

#include <eacp/Video/Decode/Player.h>
#include <NanoTest/NanoTest.h>

#include <cmath>

namespace VideoTests
{
using namespace eacp;

// Each frame carries its own index in pixel byte 0, so a test can assert which
// frame it was handed rather than inferring it from the timestamp.
struct FakeDecoder final : Video::Decoder
{
    FakeDecoder(int frameCountToUse, double frameRateToUse)
        : frameCount(frameCountToUse)
        , frameRate(frameRateToUse)
    {
    }

    bool open(const FilePath&) override { return true; }

    Video::VideoInfo info() const override
    {
        auto videoInfo = Video::VideoInfo {};
        videoInfo.width = width;
        videoInfo.height = height;
        videoInfo.frameRate = frameRate;
        videoInfo.duration = frameCount / frameRate;
        return videoInfo;
    }

    bool nextFrame(Video::VideoFrame& out) override
    {
        if (nextIndex >= frameCount)
            return false;

        auto frameInfo = Video::FrameInfo {};
        frameInfo.width = width;
        frameInfo.height = height;
        frameInfo.bytesPerRow = (std::size_t) width * 4;
        frameInfo.seconds = nextIndex / frameRate;
        frameInfo.duration = 1.0 / frameRate;

        auto pixels = Vector<std::uint8_t> {};
        pixels.resize(width * height * 4);
        pixels.data()[0] = (std::uint8_t) nextIndex;

        out = Video::VideoFrame::fromPixels(std::move(pixels), frameInfo);
        ++nextIndex;
        ++framesDecoded;
        return true;
    }

    void seek(double seconds, Video::SeekMode) override
    {
        nextIndex = (int) std::floor(seconds * frameRate);
        nextIndex = std::max(0, std::min(nextIndex, frameCount));
        ++seekCount;
    }

    int width = 2;
    int height = 2;
    int frameCount = 0;
    double frameRate = 10.0;

    int nextIndex = 0;
    std::atomic<int> framesDecoded {0};
    std::atomic<int> seekCount {0};
};

inline int indexOf(const Video::VideoFrame& frame)
{
    const auto* pixels = frame.pixels();
    return pixels != nullptr ? (int) pixels[0] : -1;
}

struct FakeStream
{
    FakeStream(int frameCount, double frameRate = 10.0, int queueDepth = 4)
    {
        auto owned = makeOwned<FakeDecoder>(frameCount, frameRate);
        decoder = owned.get();

        auto options = Video::StreamOptions {};
        options.queueDepth = queueDepth;
        stream.open(std::move(owned), options);
    }

    FakeDecoder* decoder = nullptr;
    Video::FrameStream stream;
};

constexpr auto waitTimeout = Time::MS {2000};
} // namespace VideoTests
