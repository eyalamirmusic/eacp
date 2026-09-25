#pragma once

#include "WhisperEncoder.h"

namespace WhisperEncoder
{
// The encoder WhisperEncoder::Builder records, in fp32 on the CPU: mel is
// [bands, 2 * context] band-major, the result [context, width] rows.
Vector<float>
    referenceEncoder(const Weights& weights, const Vector<float>& mel, int context);
} // namespace WhisperEncoder
