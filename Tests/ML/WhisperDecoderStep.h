#pragma once

// One Whisper tiny.en decode step at its real sizes, over deterministic
// weights: the token and its position embedded, four pre-norm layers of
// masked self-attention over a fixed 448-row KV cache, cross-attention over
// 1500 encoder rows' keys and values, and the MLP, then the final norm and the
// logits against the tied token embedding. Stateless, as phase 4's measurement
// wants it: the caches, the cross keys and values, the token, the position and
// the mask are inputs; the logits row and the key and value rows the step
// would append to the cache are the outputs. MLGraphTests builds and compiles
// it; MLTests runs it against the fp32 reference in DecoderStepReference.h.
//
// The self-attention is matmul, an additive mask through apply, softmax and
// matmul rather than scaledDotProductAttention, which takes only a causal
// flag; the cross-attention is unmasked, so it is the fused op.

#include "WhisperEncoder.h"

namespace WhisperDecoderStep
{
using eacp::Vector;
using namespace eacp::ML;
using WhisperEncoder::Norm;
using WhisperEncoder::Projection;

constexpr auto width = WhisperEncoder::width;
constexpr auto hidden = WhisperEncoder::hidden;
constexpr auto heads = WhisperEncoder::heads;
constexpr auto headWidth = WhisperEncoder::headWidth;
constexpr auto layerCount = 4;
constexpr auto vocabulary = 51864;
constexpr auto maxPositions = 448;
constexpr auto crossPositions = 1500;
constexpr auto keysPerStep = maxPositions + 1;
constexpr auto maskedScore = -10000.0f;

struct Layer
{
    Norm selfNorm;
    Projection selfQuery;
    Projection selfKey;
    Projection selfValue;
    Projection selfOut;
    Norm crossNorm;
    Projection crossQuery;
    Projection crossOut;
    Norm mlpNorm;
    Projection fc1;
    Projection fc2;
};

struct Weights
{
    Vector<float> tokens;
    Vector<float> positions;
    Vector<Layer> layers;
    Norm finalNorm;
};

inline Weights makeWeights()
{
    auto maker = WhisperEncoder::WeightMaker {4048u};
    auto weights = Weights {};
    weights.tokens = maker.values(vocabulary * width, 0.1f);
    weights.positions = maker.values(maxPositions * width, 0.3f);

    for (auto index = 0; index < layerCount; ++index)
    {
        auto layer = Layer {};
        layer.selfNorm = maker.norm();
        layer.selfQuery = maker.projection(width, width);
        layer.selfKey = maker.projection(width, width, false);
        layer.selfValue = maker.projection(width, width);
        layer.selfOut = maker.projection(width, width);
        layer.crossNorm = maker.norm();
        layer.crossQuery = maker.projection(width, width);
        layer.crossOut = maker.projection(width, width);
        layer.mlpNorm = maker.norm();
        layer.fc1 = maker.projection(hidden, width);
        layer.fc2 = maker.projection(width, hidden);
        weights.layers.add(layer);
    }

    weights.finalNorm = maker.norm();
    return weights;
}

inline const Weights& sharedWeights()
{
    static const auto weights = makeWeights();
    return weights;
}

// A step's inputs: prefix is how many cache rows hold earlier tokens, which is
// also the position this token takes.
struct StepInputs
{
    int token = 0;
    int prefix = 0;
    Vector<float> selfKeys;
    Vector<float> selfValues;
    Vector<float> crossKeys;
    Vector<float> crossValues;
};

inline const StepInputs& sharedCaches()
{
    static const auto caches = []
    {
        auto result = StepInputs {};
        result.selfKeys = TestPrograms::seededValues(
            layerCount * maxPositions * width, 9001u, 1.0f);
        result.selfValues = TestPrograms::seededValues(
            layerCount * maxPositions * width, 9002u, 1.0f);
        result.crossKeys = TestPrograms::seededValues(
            layerCount * crossPositions * width, 9003u, 1.0f);
        result.crossValues = TestPrograms::seededValues(
            layerCount * crossPositions * width, 9004u, 1.0f);
        return result;
    }();

    return caches;
}

inline StepInputs stepAt(int prefix, int token)
{
    auto inputs = sharedCaches();
    inputs.prefix = prefix;
    inputs.token = token;
    return inputs;
}

inline Vector<float> maskFor(int prefix)
{
    auto mask = Vector<float> {};

    for (auto row = 0; row < maxPositions; ++row)
        mask.add(row < prefix ? 0.0f : maskedScore);

    return mask;
}

inline Shape selfCacheShape()
{
    return {layerCount, maxPositions, width};
}

inline Shape crossCacheShape()
{
    return {layerCount, crossPositions, width};
}

class Builder
{
public:
    explicit Builder(Graph& graphToUse)
        : graph(graphToUse)
    {
    }

    void cachesAsInputs()
    {
        selfKeys = graph.input("self_keys", selfCacheShape(), DType::float16);
        selfValues = graph.input("self_values", selfCacheShape(), DType::float16);
        crossKeys = graph.input("cross_keys", crossCacheShape(), DType::float16);
        crossValues = graph.input("cross_values", crossCacheShape(), DType::float16);
    }

    void cachesAsConstants(const StepInputs& caches)
    {
        selfKeys =
            graph.halfConstant("self_keys", selfCacheShape(), caches.selfKeys);
        selfValues =
            graph.halfConstant("self_values", selfCacheShape(), caches.selfValues);
        crossKeys =
            graph.halfConstant("cross_keys", crossCacheShape(), caches.crossKeys);
        crossValues = graph.halfConstant(
            "cross_values", crossCacheShape(), caches.crossValues);
    }

    void step(const Weights& weights)
    {
        auto token = graph.input("token", {1}, DType::int32);
        auto position = graph.input("position", {1}, DType::int32);
        auto mask = graph.input("mask", {1, maxPositions}, DType::float16);

        auto ownKey = Vector<float> {0.0f};
        auto ownKeyMask = graph.halfConstant("own_key_mask", {1, 1}, ownKey);
        stepMask = graph.concat({mask, ownKeyMask}, 1);

        auto tokenTable =
            graph.halfConstant("embed_tokens", {vocabulary, width}, weights.tokens);
        auto positionTable = graph.halfConstant(
            "embed_positions", {maxPositions, width}, weights.positions);
        auto stream = graph.apply(graph.gather(tokenTable, token, 0),
                                  graph.gather(positionTable, position, 0),
                                  add);

        for (auto index = 0; index < weights.layers.size(); ++index)
            stream = layer(stream, weights.layers[index], index);

        auto normed = norm("ln", stream, weights.finalNorm);
        graph.output(graph.linear(normed, tokenTable), "logits");
        graph.output(graph.concat(newKeys, 0), "new_keys");
        graph.output(graph.concat(newValues, 0), "new_values");
    }

private:
    static constexpr auto add = [](const eacp::GPU::Float& a,
                                   const eacp::GPU::Float& b) { return a + b; };

    Tensor project(const std::string& name, Tensor x, const Projection& weights)
    {
        auto& xShape = graph.shape(x);
        auto in = static_cast<int>(xShape[xShape.rank() - 1]);
        auto out = static_cast<int>(weights.weight.size()) / in;
        auto weight =
            graph.halfConstant(name + ".weight", {out, in}, weights.weight);

        if (weights.bias.empty())
            return graph.linear(x, weight);

        auto bias = graph.halfConstant(name + ".bias", {out}, weights.bias);
        return graph.linear(x, weight, bias);
    }

    Tensor norm(const std::string& name, Tensor x, const Norm& weights)
    {
        auto gamma = graph.halfConstant(name + ".weight", {width}, weights.gamma);
        auto beta = graph.halfConstant(name + ".bias", {width}, weights.beta);
        return graph.layerNorm(x, {-1}, gamma, beta);
    }

    Tensor layerRows(Tensor stacked, int layerIndex, int rows)
    {
        auto cut =
            graph.slice(stacked, {layerIndex, 0, 0}, {layerIndex + 1, rows, width});
        return graph.reshape(cut, {rows, width});
    }

    Tensor splitHeads(Tensor rows, int count)
    {
        auto perHead = graph.reshape(rows, {count, heads, headWidth});
        return graph.transpose(perHead, {1, 0, 2});
    }

    Tensor mergeHeads(Tensor perHead)
    {
        auto byRow = graph.transpose(perHead, {1, 0, 2});
        return graph.reshape(byRow, {1, width});
    }

    Tensor selfAttention(Tensor normed, const Layer& weights, int index)
    {
        auto prefix = "decoder." + std::to_string(index) + ".self_attn.";
        auto q = project(prefix + "q_proj", normed, weights.selfQuery);
        auto k = project(prefix + "k_proj", normed, weights.selfKey);
        auto v = project(prefix + "v_proj", normed, weights.selfValue);
        newKeys.add(k);
        newValues.add(v);

        auto keys = graph.concat({layerRows(selfKeys, index, maxPositions), k}, 0);
        auto values =
            graph.concat({layerRows(selfValues, index, maxPositions), v}, 0);

        auto scale = 1.0f / std::sqrt(static_cast<float>(headWidth));
        auto scaleAndMask =
            [scale](const eacp::GPU::Float& score, const eacp::GPU::Float& masked)
        { return score * scale + masked; };

        auto scores = graph.matmul(
            splitHeads(q, 1), splitHeads(keys, keysPerStep), false, true);
        auto weightsByKey =
            graph.softmax(graph.apply(scores, stepMask, scaleAndMask), -1);
        auto attended = graph.matmul(weightsByKey, splitHeads(values, keysPerStep));
        return project(prefix + "out_proj", mergeHeads(attended), weights.selfOut);
    }

    Tensor crossAttention(Tensor normed, const Layer& weights, int index)
    {
        auto prefix = "decoder." + std::to_string(index) + ".encoder_attn.";
        auto q = project(prefix + "q_proj", normed, weights.crossQuery);
        auto keys =
            splitHeads(layerRows(crossKeys, index, crossPositions), crossPositions);
        auto values = splitHeads(layerRows(crossValues, index, crossPositions),
                                 crossPositions);
        auto attended =
            graph.scaledDotProductAttention(splitHeads(q, 1), keys, values, false);
        return project(prefix + "out_proj", mergeHeads(attended), weights.crossOut);
    }

    Tensor layer(Tensor stream, const Layer& weights, int index)
    {
        auto prefix = "decoder." + std::to_string(index) + ".";
        auto selfIn = norm(prefix + "self_attn_ln", stream, weights.selfNorm);
        stream = graph.apply(stream, selfAttention(selfIn, weights, index), add);

        auto crossIn = norm(prefix + "encoder_attn_ln", stream, weights.crossNorm);
        stream = graph.apply(stream, crossAttention(crossIn, weights, index), add);

        auto mlpIn = norm(prefix + "final_ln", stream, weights.mlpNorm);
        auto expanded = graph.gelu(project(prefix + "fc1", mlpIn, weights.fc1));
        return graph.apply(
            stream, project(prefix + "fc2", expanded, weights.fc2), add);
    }

    Graph& graph;
    Tensor selfKeys;
    Tensor selfValues;
    Tensor crossKeys;
    Tensor crossValues;
    Tensor stepMask;
    Vector<Tensor> newKeys;
    Vector<Tensor> newValues;
};

inline Graph stepGraph(const Weights& weights)
{
    auto graph = Graph {};
    auto builder = Builder {graph};
    builder.cachesAsInputs();
    builder.step(weights);
    return graph;
}

inline Graph sharedStepGraph()
{
    return stepGraph(sharedWeights());
}

// The same step with the caches baked into the blob, so nothing but the token,
// the position and the mask crosses into a prediction: a bound on what a
// stateful step, whose caches never leave Core ML, could cost. It computes the
// step for sharedCaches() only.
inline Graph residentCacheStepGraph()
{
    auto graph = Graph {};
    auto builder = Builder {graph};
    builder.cachesAsConstants(sharedCaches());
    builder.step(sharedWeights());
    return graph;
}
} // namespace WhisperDecoderStep
