#include "DecoderStepReference.h"
#include "EncoderReference.h"

namespace WhisperDecoderStep
{
namespace
{
using WhisperEncoder::Reference::addInto;
using WhisperEncoder::Reference::gelu;
using WhisperEncoder::Reference::layerNorm;
using WhisperEncoder::Reference::linear;
using WhisperEncoder::Reference::Rows;
using WhisperEncoder::Reference::softmax;

Rows embedded(const Weights& weights, const StepInputs& inputs)
{
    auto row = Rows {1, width};

    for (auto c = 0; c < width; ++c)
        row.values[c] = weights.tokens[inputs.token * width + c]
                        + weights.positions[inputs.prefix * width + c];

    return row;
}

Rows cacheRows(const Vector<float>& stacked, int layer, int capacity, int count)
{
    auto rows = Rows {count, width};
    auto* first = stacked.data() + layer * capacity * width;
    std::copy_n(first, count * width, rows.values.data());
    return rows;
}

Rows withRow(const Rows& rows, const Rows& last)
{
    auto result = Rows {rows.count + 1, width};
    std::copy_n(rows.values.data(), rows.values.size(), result.values.data());
    std::copy_n(last.values.data(), width, result.row(rows.count));
    return result;
}

Rows attend(const Rows& query, const Rows& keys, const Rows& values)
{
    auto scale = 1.0f / std::sqrt((float) headWidth);
    auto out = Rows {1, width};
    auto scores = Vector<float> {};

    for (auto head = 0; head < heads; ++head)
    {
        auto first = head * headWidth;
        scores.clear();

        for (auto j = 0; j < keys.count; ++j)
        {
            auto dot = 0.0f;

            for (auto d = 0; d < headWidth; ++d)
                dot += query.row(0)[first + d] * keys.row(j)[first + d];

            scores.add(dot * scale);
        }

        softmax(scores);
        auto* target = out.row(0) + first;

        for (auto j = 0; j < keys.count; ++j)
            for (auto d = 0; d < headWidth; ++d)
                target[d] += scores[j] * values.row(j)[first + d];
    }

    return out;
}

struct Appended
{
    Vector<float> keys;
    Vector<float> values;
};

Rows selfAttention(const Rows& normed,
                   const Layer& layer,
                   const StepInputs& inputs,
                   int index,
                   Appended& appended)
{
    auto key = linear(normed, layer.selfKey, width);
    auto value = linear(normed, layer.selfValue, width);
    appended.keys.addFrom(key.values);
    appended.values.addFrom(value.values);

    auto keys =
        withRow(cacheRows(inputs.selfKeys, index, maxPositions, inputs.prefix), key);
    auto values = withRow(
        cacheRows(inputs.selfValues, index, maxPositions, inputs.prefix), value);
    auto attended = attend(linear(normed, layer.selfQuery, width), keys, values);
    return linear(attended, layer.selfOut, width);
}

Rows crossAttention(const Rows& normed,
                    const Layer& layer,
                    const StepInputs& inputs,
                    int index)
{
    auto keys = cacheRows(inputs.crossKeys, index, crossPositions, crossPositions);
    auto values =
        cacheRows(inputs.crossValues, index, crossPositions, crossPositions);
    auto attended = attend(linear(normed, layer.crossQuery, width), keys, values);
    return linear(attended, layer.crossOut, width);
}
} // namespace

StepOutputs referenceStep(const Weights& weights, const StepInputs& inputs)
{
    auto stream = embedded(weights, inputs);
    auto appended = Appended {};

    for (auto index = 0; index < weights.layers.size(); ++index)
    {
        auto& layer = weights.layers[index];
        addInto(
            stream,
            selfAttention(
                layerNorm(stream, layer.selfNorm), layer, inputs, index, appended));
        addInto(stream,
                crossAttention(
                    layerNorm(stream, layer.crossNorm), layer, inputs, index));

        auto expanded = linear(layerNorm(stream, layer.mlpNorm), layer.fc1, hidden);
        gelu(expanded);
        addInto(stream, linear(expanded, layer.fc2, width));
    }

    auto tied = Projection {weights.tokens, {}};
    auto logits = linear(layerNorm(stream, weights.finalNorm), tied, vocabulary);
    return {logits.values, appended.keys, appended.values};
}
} // namespace WhisperDecoderStep
