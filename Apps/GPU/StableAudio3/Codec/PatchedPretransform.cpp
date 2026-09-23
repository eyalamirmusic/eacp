#include "PatchedPretransform.h"

#include <algorithm>

namespace eacp::SA3Codec
{
namespace
{
float sampleAt(const std::vector<float>& channel, int index)
{
    return index < (int) channel.size() ? channel[(std::size_t) index] : 0.f;
}
}

HostMatrix patchedPretransformEncode(const StereoWaveform& waveform, int patchSize)
{
    auto sampleCount = (int) std::max(waveform.left.size(), waveform.right.size());
    auto remainder = sampleCount % patchSize;
    auto paddedCount = remainder == 0 ? sampleCount : sampleCount + (patchSize - remainder);
    auto frames = paddedCount / patchSize;

    auto result = HostMatrix::zeros(frames, 2 * patchSize);

    for (auto frame = 0; frame < frames; ++frame)
        for (auto h = 0; h < patchSize; ++h)
        {
            auto sampleIndex = frame * patchSize + h;
            result.at(frame, h) = sampleAt(waveform.left, sampleIndex);
            result.at(frame, patchSize + h) = sampleAt(waveform.right, sampleIndex);
        }

    return result;
}

StereoWaveform patchedPretransformDecode(const HostMatrix& patched,
                                         int sampleCount,
                                         int patchSize)
{
    auto naturalLength = patched.rows * patchSize;
    auto outLength = sampleCount > 0 ? std::min(sampleCount, naturalLength) : naturalLength;

    auto result = StereoWaveform {};
    result.left.resize((std::size_t) outLength);
    result.right.resize((std::size_t) outLength);

    for (auto frame = 0; frame < patched.rows; ++frame)
        for (auto h = 0; h < patchSize; ++h)
        {
            auto sampleIndex = frame * patchSize + h;

            if (sampleIndex >= outLength)
                continue;

            result.left[(std::size_t) sampleIndex] = patched.at(frame, h);
            result.right[(std::size_t) sampleIndex] = patched.at(frame, patchSize + h);
        }

    return result;
}
}
