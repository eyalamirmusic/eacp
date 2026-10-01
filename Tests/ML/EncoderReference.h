#pragma once

#include "WhisperEncoder.h"

namespace WhisperEncoder
{
// The encoder WhisperEncoder::Builder records, in fp32 on the CPU: mel is
// [bands, 2 * context] band-major, the result [context, width] rows.
Vector<float>
    referenceEncoder(const Weights& weights, const Vector<float>& mel, int context);

// The fp32 pieces the encoder reference is made of, shared with the decode
// step's.
namespace Reference
{
struct Rows
{
    Rows(int countToUse, int widthToUse)
        : count(countToUse)
        , width(widthToUse)
    {
        values.resize(count * width, 0.0f);
    }

    float* row(int index) { return values.data() + index * width; }
    const float* row(int index) const { return values.data() + index * width; }

    int count = 0;
    int width = 0;
    Vector<float> values;
};

Rows linear(const Rows& x, const Projection& projection, int out);
void gelu(Rows& x);
Rows layerNorm(const Rows& x, const Norm& norm);
void addInto(Rows& stream, const Rows& other);
void softmax(Vector<float>& scores);
} // namespace Reference
} // namespace WhisperEncoder
