#pragma once

#include "PatchedPretransform.h"

#include <string>

namespace eacp::SA3Codec
{
bool writeWavFile(const std::string& path, const StereoWaveform& waveform, int sampleRate);
}
