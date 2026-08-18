#include "AudioRing.h"

#include <algorithm>

namespace eacp::Video
{
namespace
{
int roundUpToPowerOfTwo(int value)
{
    auto result = 1;

    while (result < value)
        result *= 2;

    return result;
}
} // namespace

void AudioRing::prepare(int numChannels, int capacityFrames)
{
    channelCount = std::max(1, numChannels);
    capacity = roundUpToPowerOfTwo(std::max(1, capacityFrames));

    samples.resize(channelCount * capacity);
    samples.fill(0.f);

    scratch.resize(channelCount * capacity);
    scratchChannels.resize(channelCount);

    for (auto channel = 0; channel < channelCount; ++channel)
        scratchChannels[channel] = scratch.data() + channel * capacity;

    writeIndex.store(0, std::memory_order_relaxed);
    readIndex.store(0, std::memory_order_relaxed);
    dropped.store(0, std::memory_order_relaxed);
}

bool AudioRing::write(const AudioBuffer& buffer) noexcept
{
    if (capacity == 0 || !buffer.isValid())
        return false;

    auto write = writeIndex.load(std::memory_order_relaxed);
    auto read = readIndex.load(std::memory_order_acquire);
    auto frames = buffer.numFrames;

    if (frames > capacity - static_cast<int>(write - read))
    {
        dropped.fetch_add(frames, std::memory_order_relaxed);
        return false;
    }

    auto offset = static_cast<int>(write & (capacity - 1));
    auto untilWrap = std::min(frames, capacity - offset);

    for (auto channel = 0; channel < channelCount; ++channel)
    {
        auto source = buffer.channel(std::min(channel, buffer.numChannels - 1));
        auto* destination = samples.data() + channel * capacity;

        std::copy_n(source.data(), untilWrap, destination + offset);
        std::copy_n(source.data() + untilWrap, frames - untilWrap, destination);
    }

    writeIndex.store(write + frames, std::memory_order_release);
    return true;
}

AudioBuffer AudioRing::read(int maxFrames) noexcept
{
    if (capacity == 0)
        return {};

    auto read = readIndex.load(std::memory_order_relaxed);
    auto write = writeIndex.load(std::memory_order_acquire);

    auto frames = std::min({maxFrames, capacity, static_cast<int>(write - read)});

    if (frames <= 0)
        return {};

    auto offset = static_cast<int>(read & (capacity - 1));
    auto untilWrap = std::min(frames, capacity - offset);

    for (auto channel = 0; channel < channelCount; ++channel)
    {
        const auto* source = samples.data() + channel * capacity;
        auto* destination = scratch.data() + channel * capacity;

        std::copy_n(source + offset, untilWrap, destination);
        std::copy_n(source, frames - untilWrap, destination + untilWrap);
    }

    readIndex.store(read + frames, std::memory_order_release);
    return {scratchChannels.data(), channelCount, frames};
}

std::int64_t AudioRing::framesRead() const noexcept
{
    return readIndex.load(std::memory_order_relaxed);
}

int AudioRing::available() const noexcept
{
    auto write = writeIndex.load(std::memory_order_acquire);
    auto read = readIndex.load(std::memory_order_relaxed);
    return static_cast<int>(write - read);
}

int AudioRing::droppedFrames() const noexcept
{
    return dropped.load(std::memory_order_relaxed);
}

} // namespace eacp::Video
