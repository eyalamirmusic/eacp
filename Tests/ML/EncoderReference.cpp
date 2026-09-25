#include "EncoderReference.h"

#include <algorithm>
#include <cmath>

namespace WhisperEncoder
{
namespace
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

Vector<float> transposed(const Vector<float>& matrix, int rows, int columns)
{
    auto result = Vector<float> {};
    result.resize(rows * columns, 0.0f);

    for (auto row = 0; row < rows; ++row)
        for (auto column = 0; column < columns; ++column)
            result[column * rows + row] = matrix[row * columns + column];

    return result;
}

Rows linear(const Rows& x, const Projection& projection, int out)
{
    auto weightByInput = transposed(projection.weight, out, x.width);
    auto y = Rows {x.count, out};

    for (auto index = 0; index < x.count; ++index)
    {
        auto* target = y.row(index);
        auto* source = x.row(index);

        if (!projection.bias.empty())
            std::copy_n(projection.bias.data(), out, target);

        for (auto in = 0; in < x.width; ++in)
        {
            auto scale = source[in];
            auto* weights = weightByInput.data() + in * out;

            for (auto o = 0; o < out; ++o)
                target[o] += scale * weights[o];
        }
    }

    return y;
}

void gelu(Rows& x)
{
    for (auto& value: x.values)
        value = 0.5f * value * (1.0f + std::erf(value / std::sqrt(2.0f)));
}

Rows layerNorm(const Rows& x, const Norm& norm)
{
    constexpr auto epsilon = 1e-5;
    auto y = Rows {x.count, x.width};

    for (auto index = 0; index < x.count; ++index)
    {
        auto* in = x.row(index);
        auto mean = 0.0;

        for (auto c = 0; c < x.width; ++c)
            mean += in[c];

        mean /= x.width;
        auto variance = 0.0;

        for (auto c = 0; c < x.width; ++c)
            variance += (in[c] - mean) * (in[c] - mean);

        auto scale = 1.0 / std::sqrt(variance / x.width + epsilon);
        auto* out = y.row(index);

        for (auto c = 0; c < x.width; ++c)
            out[c] = (float) ((in[c] - mean) * scale * norm.gamma[c] + norm.beta[c]);
    }

    return y;
}

void addInto(Rows& stream, const Rows& other)
{
    for (auto index = 0; index < stream.values.size(); ++index)
        stream.values[index] += other.values[index];
}

// A kernel-3, padding-1 convolution's input windows as rows, so that the
// convolution is a linear over them: the window of output t holds input
// frame t * stride + k - 1 of channel c at c * kernel + k, the order of the
// [out, in, kernel] weight flattened.
Rows windows(const Rows& frames, int stride, int outLength)
{
    auto unfolded = Rows {outLength, frames.width * kernel};

    for (auto t = 0; t < outLength; ++t)
        for (auto k = 0; k < kernel; ++k)
        {
            auto frame = t * stride + k - 1;

            if (frame < 0 || frame >= frames.count)
                continue;

            for (auto c = 0; c < frames.width; ++c)
                unfolded.row(t)[c * kernel + k] = frames.row(frame)[c];
        }

    return unfolded;
}

void softmax(Vector<float>& scores)
{
    auto top = *std::max_element(scores.begin(), scores.end());
    auto total = 0.0;

    for (auto& score: scores)
    {
        score = std::exp(score - top);
        total += score;
    }

    for (auto& score: scores)
        score = (float) (score / total);
}

Rows attention(const Rows& q, const Rows& k, const Rows& v)
{
    auto length = q.count;
    auto scale = 1.0f / std::sqrt((float) headWidth);
    auto out = Rows {length, width};
    auto scores = Vector<float> {};
    auto keysByDepth = Vector<float> {};
    keysByDepth.resize(headWidth * length, 0.0f);

    for (auto head = 0; head < heads; ++head)
    {
        auto first = head * headWidth;

        for (auto j = 0; j < length; ++j)
            for (auto d = 0; d < headWidth; ++d)
                keysByDepth[d * length + j] = k.row(j)[first + d];

        for (auto i = 0; i < length; ++i)
        {
            scores.clear();
            scores.resize(length, 0.0f);
            auto* query = q.row(i) + first;

            for (auto d = 0; d < headWidth; ++d)
            {
                auto weight = query[d] * scale;
                auto* keys = keysByDepth.data() + d * length;

                for (auto j = 0; j < length; ++j)
                    scores[j] += weight * keys[j];
            }

            softmax(scores);
            auto* target = out.row(i) + first;

            for (auto j = 0; j < length; ++j)
            {
                auto probability = scores[j];
                auto* value = v.row(j) + first;

                for (auto d = 0; d < headWidth; ++d)
                    target[d] += probability * value[d];
            }
        }
    }

    return out;
}

Rows melFrames(const Vector<float>& mel, int frameCount)
{
    auto frames = Rows {frameCount, bands};

    for (auto band = 0; band < bands; ++band)
        for (auto frame = 0; frame < frameCount; ++frame)
            frames.row(frame)[band] = mel[band * frameCount + frame];

    return frames;
}

void encoderLayer(Rows& stream, const Layer& layer)
{
    auto normed = layerNorm(stream, layer.attentionNorm);
    auto attended = attention(linear(normed, layer.query, width),
                              linear(normed, layer.key, width),
                              linear(normed, layer.value, width));
    addInto(stream, linear(attended, layer.out, width));

    auto expanded = linear(layerNorm(stream, layer.mlpNorm), layer.fc1, hidden);
    gelu(expanded);
    addInto(stream, linear(expanded, layer.fc2, width));
}
} // namespace

Vector<float>
    referenceEncoder(const Weights& weights, const Vector<float>& mel, int context)
{
    auto frames = melFrames(mel, 2 * context);

    auto first = linear(windows(frames, 1, frames.count), weights.conv1, width);
    gelu(first);

    auto stream = linear(windows(first, 2, context), weights.conv2, width);
    gelu(stream);

    for (auto index = 0; index < stream.values.size(); ++index)
        stream.values[index] += weights.positions[index];

    for (const auto& layer: weights.layers)
        encoderLayer(stream, layer);

    return layerNorm(stream, weights.finalNorm).values;
}
} // namespace WhisperEncoder
