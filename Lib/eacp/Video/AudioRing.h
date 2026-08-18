#pragma once

#include "Audio.h"

#include <atomic>

namespace eacp::Video
{

// The lock-free handover between the audio thread pushing blocks and the
// recorder's drain feeding them to the encoder. One producer, one consumer.
//
// EA::Fifo is the wrong shape here: it hands the reader the most recent value
// and drops what came between, which is exactly what a continuous audio stream
// cannot survive. This keeps every sample instead, and when the reader falls
// far enough behind that a block will not fit, the block is dropped WHOLE --
// half of one written into the file is a click -- and counted.
class AudioRing
{
public:
    // Rounds the capacity up to a power of two. Call before either thread runs.
    void prepare(int numChannels, int capacityFrames);

    // Producer. Never allocates, never blocks. A mono block feeds every
    // channel; a wider one has its extra channels dropped.
    bool write(const AudioBuffer& buffer) noexcept;

    // Consumer. The view stays valid until the next read().
    AudioBuffer read(int maxFrames) noexcept;

    // Frames handed to the consumer so far, which is also the index of the
    // frame the next read() starts at.
    std::int64_t framesRead() const noexcept;

    int available() const noexcept;

    // Frames thrown away by refused writes. A producer that retries, rather
    // than moving on as the recorder does, has its retries counted here too.
    int droppedFrames() const noexcept;

private:
    Vector<float> samples;
    Vector<float> scratch;
    Vector<const float*> scratchChannels;

    int channelCount = 0;
    int capacity = 0;

    std::atomic<std::int64_t> writeIndex {0};
    std::atomic<std::int64_t> readIndex {0};
    std::atomic<int> dropped {0};
};

} // namespace eacp::Video
