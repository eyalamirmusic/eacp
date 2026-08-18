#include "Common.h"

#include <eacp/Video/AudioRing.h>
#include <eacp/Video/Encoder.h>

#include <atomic>
#include <cmath>
#include <thread>

using namespace nano;
using namespace eacp;

namespace
{
// Every sample carries its own position in the stream, offset per channel, so a
// reader can prove exactly which frames it was handed and in what order.
float rampValue(int channel, std::int64_t frame)
{
    return static_cast<float>(frame * 10 + channel);
}

class RampBlock
{
public:
    RampBlock(int numChannels, int numFrames)
        : channelCount(numChannels)
        , frameCount(numFrames)
    {
        samples.resize(channelCount * frameCount);
        channels.resize(channelCount);

        for (auto channel = 0; channel < channelCount; ++channel)
            channels[channel] = samples.data() + channel * frameCount;
    }

    Video::AudioBuffer at(std::int64_t startFrame)
    {
        for (auto channel = 0; channel < channelCount; ++channel)
            for (auto frame = 0; frame < frameCount; ++frame)
                samples[channel * frameCount + frame] =
                    rampValue(channel, startFrame + frame);

        return {channels.data(), channelCount, frameCount};
    }

private:
    Vector<float> samples;
    Vector<const float*> channels;
    int channelCount = 0;
    int frameCount = 0;
};

bool matchesRamp(const Video::AudioBuffer& block, std::int64_t startFrame)
{
    for (auto channel = 0; channel < block.numChannels; ++channel)
        for (auto frame = 0; frame < block.numFrames; ++frame)
            if (block.channel(channel)[frame]
                != rampValue(channel, startFrame + frame))
                return false;

    return true;
}
} // namespace

auto tRingReadsBackWhatWasWritten = test("AudioRing/readsBackWhatWasWritten") = []
{
    auto ring = Video::AudioRing {};
    ring.prepare(2, 1024);

    auto block = RampBlock {2, 64};
    check(ring.write(block.at(0)));
    check(ring.available() == 64);

    auto read = ring.read(64);
    check(read.numChannels == 2);
    check(read.numFrames == 64);
    check(matchesRamp(read, 0));

    check(ring.available() == 0);
    check(!ring.read(64).isValid());
};

// Blocks whose size does not divide the capacity, so every pass lands at a
// different offset and the copies straddle the end of the buffer.
auto tRingWrapsAroundTheEnd = test("AudioRing/wrapsAroundTheEnd") = []
{
    auto ring = Video::AudioRing {};
    ring.prepare(2, 8);

    auto block = RampBlock {2, 3};
    auto intact = true;

    for (auto pass = std::int64_t {0}; pass < 20; ++pass)
    {
        check(ring.write(block.at(pass * 3)));
        intact = intact && matchesRamp(ring.read(3), pass * 3);
    }

    check(intact);
    check(ring.droppedFrames() == 0);
};

// A block that does not fit is refused whole: what is already in the ring must
// still read back exactly, because a half-written block is a click.
auto tRingDropsBlocksThatDoNotFit = test("AudioRing/dropsBlocksThatDoNotFit") = []
{
    auto ring = Video::AudioRing {};
    ring.prepare(1, 8);

    auto block = RampBlock {1, 8};
    check(ring.write(block.at(0)));

    auto overflow = RampBlock {1, 4};
    check(!ring.write(overflow.at(1000)));
    check(ring.droppedFrames() == 4);

    check(matchesRamp(ring.read(8), 0));
};

auto tRingReadsWhatIsAvailable = test("AudioRing/readsAtMostWhatIsAvailable") = []
{
    auto ring = Video::AudioRing {};
    ring.prepare(2, 512);

    auto block = RampBlock {2, 100};
    check(ring.write(block.at(0)));

    auto read = ring.read(1000);
    check(read.numFrames == 100);
};

auto tRingMonoFeedsEveryChannel = test("AudioRing/monoBlockFeedsEveryChannel") = []
{
    auto ring = Video::AudioRing {};
    ring.prepare(2, 128);

    auto mono = RampBlock {1, 32};
    check(ring.write(mono.at(0)));

    auto read = ring.read(32);
    check(read.numChannels == 2);

    for (auto frame = 0; frame < 32; ++frame)
        check(read.channel(1)[frame] == read.channel(0)[frame]);
};

// The drain timestamps each block by the frame index the ring is about to hand
// it, so that count has to survive every read.
auto tRingTracksFramesRead = test("AudioRing/framesReadTracksTheStream") = []
{
    auto ring = Video::AudioRing {};
    ring.prepare(1, 256);

    auto block = RampBlock {1, 40};
    check(ring.framesRead() == 0);

    check(ring.write(block.at(0)));
    ring.read(30);
    check(ring.framesRead() == 30);

    ring.read(30);
    check(ring.framesRead() == 40);
};

// The one that matters: an audio thread and a drain running at once, with the
// producer waiting rather than dropping, so any lost, duplicated or reordered
// frame shows up as a break in the ramp.
auto tRingSurvivesConcurrentAccess =
    test("AudioRing/survivesProducerAndConsumer") = []
{
    constexpr auto blockFrames = 128;
    constexpr auto totalFrames = std::int64_t {blockFrames} * 900;

    auto ring = Video::AudioRing {};
    ring.prepare(2, 2048);

    auto abort = std::atomic<bool> {false};
    auto refused = std::atomic<int> {0};

    auto producer = std::thread(
        [&]
        {
            auto block = RampBlock {2, blockFrames};

            for (auto frame = std::int64_t {0}; frame < totalFrames && !abort.load();
                 frame += blockFrames)
                while (!ring.write(block.at(frame)) && !abort.load())
                {
                    ++refused;
                    std::this_thread::yield();
                }
        });

    auto received = std::int64_t {0};
    auto intact = true;
    auto deadline = Time::Deadline {Time::MS {10'000}};

    while (received < totalFrames && !deadline.expired())
    {
        auto block = ring.read(333);

        if (!block.isValid())
        {
            std::this_thread::yield();
            continue;
        }

        intact = intact && matchesRamp(block, received);
        received += block.numFrames;
    }

    abort = true;
    producer.join();

    check(intact);
    check(received == totalFrames);

    // Every refusal threw the whole block away, which is what the counter is
    // for. Nothing else did: a producer that retries, as this one does, ends up
    // with exactly its retries counted and no frames actually lost.
    check(ring.droppedFrames() == refused * blockFrames);
};

auto tAudioTimeAdvancesBySampleCount =
    test("AudioTimeline/advancesBySampleCount") = []
{
    auto spec = Video::AudioSpec {};
    spec.sampleRate = 48'000;

    check(Video::audioTimeFor(0, 1.5, spec) == 1.5);
    check(std::abs(Video::audioTimeFor(48'000, 1.5, spec) - 2.5) < 1e-9);
    check(std::abs(Video::audioTimeFor(24'000, 0.0, spec) - 0.5) < 1e-9);
};

// The output latency is the gap between what the app pushed and what the
// camera saw, so it delays the track rather than shifting the anchor.
auto tAudioTimeDelaysByLatency = test("AudioTimeline/delaysByOutputLatency") = []
{
    auto spec = Video::AudioSpec {};
    spec.sampleRate = 48'000;
    spec.latencyFrames = 480;

    check(std::abs(Video::audioTimeFor(0, 0.0, spec) - 0.01) < 1e-9);
    check(std::abs(Video::audioTimeFor(48'000, 0.0, spec) - 1.01) < 1e-9);
};

// The encoder OUTLIVES a recording -- a second start() reuses it -- so what
// finish() leaves behind is what the NEXT recording's drain thread reads to
// decide whether there is a track to write into.
auto tEncoderStopsAcceptingAudioOnceFinished =
    test("Encoder/stopsAcceptingAudioOnceFinished") = []
{
    auto spec = Video::EncoderSpec {};
    spec.video.width = 320;
    spec.video.height = 240;
    spec.video.fps = 10;
    // Non-zero: AVFoundation refuses a zero average bitrate outright, and every
    // real caller computes one (VideoRecorder::specFor).
    spec.video.bitrate = 320 * 240 * 8;
    spec.audio = Video::AudioSpec {};

    auto encoder = Video::makeEncoder();
    auto image = Graphics::Image {spec.video.width, spec.video.height};
    auto block = RampBlock {spec.audio->numChannels, 1024};

    auto recordOneTake = [&](const FilePath& path)
    {
        check(encoder->begin(path, spec));
        check(encoder->acceptsAudio());

        encoder->waitUntilReady(Time::MS {5000});
        encoder->appendImage(image, 0.0);
        encoder->appendAudio(block.at(0), 0.0);

        encoder->finish().waitFor(Time::MS {60'000});

        // THE REGRESSION. A finished track left advertised is one the next
        // recording's drain thread starts feeding before the screen tier's
        // asynchronous begin() has published a track of its own -- and that
        // begin() then releases the input out from under it.
        check(!encoder->acceptsAudio());
    };

    recordOneTake(FilePath::tempDirectory() / "eacp-encoder-reuse-a.mp4");
    recordOneTake(FilePath::tempDirectory() / "eacp-encoder-reuse-b.mp4");
};
