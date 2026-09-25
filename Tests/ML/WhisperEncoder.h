#pragma once

// The Whisper tiny.en encoder at its real sizes, over deterministic weights
// rather than the model's: 80 mel bands, width 384, six heads, four layers,
// the eighteen audio contexts the live path uses, and a key projection with no
// bias. MLGraphTests builds and compiles it; MLTests runs it against the fp32
// reference in EncoderReference.h.

#include "TestPrograms.h"

#include <string>

namespace WhisperEncoder
{
using eacp::Vector;
using namespace eacp::ML;

constexpr auto bands = 80;
constexpr auto width = 384;
constexpr auto hidden = 4 * width;
constexpr auto heads = 6;
constexpr auto headWidth = width / heads;
constexpr auto layerCount = 4;
constexpr auto kernel = 3;
constexpr auto maxContext = 1500;
constexpr auto maxFrames = 2 * maxContext;

inline Vector<int> audioContexts()
{
    auto contexts = Vector<int> {};

    for (auto context = 448; context < maxContext; context += 64)
        contexts.add(context);

    contexts.add(maxContext);
    return contexts;
}

struct Projection
{
    Vector<float> weight;
    Vector<float> bias;
};

struct Norm
{
    Vector<float> gamma;
    Vector<float> beta;
};

struct Layer
{
    Norm attentionNorm;
    Projection query;
    Projection key;
    Projection value;
    Projection out;
    Norm mlpNorm;
    Projection fc1;
    Projection fc2;
};

struct Weights
{
    Projection conv1;
    Projection conv2;
    Vector<float> positions;
    Vector<Layer> layers;
    Norm finalNorm;
};

class WeightMaker
{
public:
    Projection projection(int out, int in, bool withBias = true)
    {
        auto spread = 1.0f / std::sqrt(static_cast<float>(in));
        auto result = Projection {};
        result.weight = TestPrograms::seededValues(out * in, nextSeed(), spread);

        if (withBias)
            result.bias = TestPrograms::seededValues(out, nextSeed(), 0.02f);

        return result;
    }

    Norm norm()
    {
        auto result = Norm {};
        result.gamma = TestPrograms::seededValues(width, nextSeed(), 0.1f);
        result.beta = TestPrograms::seededValues(width, nextSeed(), 0.05f);

        for (auto& value: result.gamma)
            value = halfToFloat(floatToHalf(value + 1.0f));

        return result;
    }

    Vector<float> values(int count, float spread)
    {
        return TestPrograms::seededValues(count, nextSeed(), spread);
    }

private:
    unsigned nextSeed() { return seed++; }

    unsigned seed = 2026u;
};

inline Weights makeWeights()
{
    auto maker = WeightMaker {};
    auto weights = Weights {};
    weights.conv1 = maker.projection(width, bands * kernel);
    weights.conv2 = maker.projection(width, width * kernel);
    weights.positions = maker.values(maxContext * width, 0.3f);

    for (auto index = 0; index < layerCount; ++index)
    {
        auto layer = Layer {};
        layer.attentionNorm = maker.norm();
        layer.query = maker.projection(width, width);
        layer.key = maker.projection(width, width, false);
        layer.value = maker.projection(width, width);
        layer.out = maker.projection(width, width);
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

// A mel window of maxFrames frames per band, band-major, the way the kernel
// path's binding holds it; a context of N reads the first 2N of each band.
inline Vector<float> melWindow()
{
    return TestPrograms::seededValues(bands * maxFrames, 7777u, 0.7f);
}

inline Vector<float> melFor(const Vector<float>& window, int context)
{
    auto frames = 2 * context;
    auto mel = Vector<float> {};

    for (auto band = 0; band < bands; ++band)
        for (auto frame = 0; frame < frames; ++frame)
            mel.add(window[band * maxFrames + frame]);

    return mel;
}

inline Shape melShape(int context)
{
    return {1, bands, 2 * context};
}

class Builder
{
public:
    explicit Builder(Graph& graphToUse)
        : graph(graphToUse)
    {
    }

    Tensor encode(Tensor mel, const Weights& weights)
    {
        auto first = graph.gelu(conv("conv1", mel, weights.conv1, bands, 1));
        auto second = graph.gelu(conv("conv2", first, weights.conv2, width, 2));
        auto frames = graph.transpose(second, {0, 2, 1});
        auto rows = graph.reshape(frames, {Shape::unknown, width});
        auto table = graph.halfConstant(
            "positional_embedding", {maxContext, width}, weights.positions);
        auto stream = graph.apply(rows, graph.sliceLike(table, rows), add);

        for (auto index = 0; index < weights.layers.size(); ++index)
            stream = layer(stream, weights.layers[index], index);

        return norm("ln_post", stream, weights.finalNorm);
    }

private:
    static constexpr auto add = [](const eacp::GPU::Float& a,
                                   const eacp::GPU::Float& b) { return a + b; };

    Tensor conv(const std::string& name,
                Tensor x,
                const Projection& weights,
                int in,
                int stride)
    {
        auto weight = graph.halfConstant(
            name + ".weight", {width, in, kernel}, weights.weight);
        auto bias = graph.halfConstant(name + ".bias", {width}, weights.bias);
        return graph.conv(x, weight, bias, stride, 1);
    }

    Tensor project(const std::string& name,
                   Tensor x,
                   const Projection& weights,
                   int out,
                   int in)
    {
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

    Tensor splitHeads(Tensor rows)
    {
        auto perHead = graph.reshape(rows, {Shape::unknown, heads, headWidth});
        return graph.transpose(perHead, {1, 0, 2});
    }

    Tensor mergeHeads(Tensor perHead)
    {
        auto byRow = graph.transpose(perHead, {1, 0, 2});
        return graph.reshape(byRow, {Shape::unknown, width});
    }

    Tensor layer(Tensor stream, const Layer& weights, int index)
    {
        auto prefix = "blocks." + std::to_string(index) + ".";
        auto normed = norm(prefix + "attn_ln", stream, weights.attentionNorm);
        auto q = project(prefix + "attn.query", normed, weights.query, width, width);
        auto k = project(prefix + "attn.key", normed, weights.key, width, width);
        auto v = project(prefix + "attn.value", normed, weights.value, width, width);
        auto attended = graph.scaledDotProductAttention(
            splitHeads(q), splitHeads(k), splitHeads(v), false);
        auto projected = project(
            prefix + "attn.out", mergeHeads(attended), weights.out, width, width);
        stream = graph.apply(stream, projected, add);

        auto mlpIn = norm(prefix + "mlp_ln", stream, weights.mlpNorm);
        auto expanded =
            graph.gelu(project(prefix + "mlp.0", mlpIn, weights.fc1, hidden, width));
        auto contracted =
            project(prefix + "mlp.2", expanded, weights.fc2, width, hidden);
        return graph.apply(stream, contracted, add);
    }

    Graph& graph;
};

// The encoder over every context in contexts, defaultContext first; one
// context makes a fixed-shape program.
inline Graph encoderGraph(const Weights& weights,
                          const Vector<int>& contexts,
                          int defaultContext)
{
    auto graph = Graph {};
    auto shapes = Vector<Shape> {};

    for (auto context: contexts)
        shapes.add(melShape(context));

    auto mel =
        contexts.size() > 1
            ? graph.input("mel", melShape(defaultContext), shapes, DType::float16)
            : graph.input("mel", melShape(defaultContext), DType::float16);

    auto builder = Builder {graph};
    graph.output(builder.encode(mel, weights), "rows");
    return graph;
}

inline Graph enumeratedEncoderGraph()
{
    return encoderGraph(sharedWeights(), audioContexts(), maxContext);
}

inline Graph fixedEncoderGraph(int context)
{
    return encoderGraph(sharedWeights(), {context}, context);
}
} // namespace WhisperEncoder
