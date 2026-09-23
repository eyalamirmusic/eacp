#pragma once

#include "HostMatrix.h"

#include <vector>

namespace eacp::SA3Codec
{
constexpr auto patchedPretransformPatchSize = 256;

struct StereoWaveform
{
    std::vector<float> left;
    std::vector<float> right;
};

HostMatrix patchedPretransformEncode(const StereoWaveform& waveform,
                                     int patchSize = patchedPretransformPatchSize);

StereoWaveform patchedPretransformDecode(const HostMatrix& patched,
                                         int sampleCount,
                                         int patchSize = patchedPretransformPatchSize);
}
