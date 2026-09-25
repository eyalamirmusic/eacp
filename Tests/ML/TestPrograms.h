#pragma once

// The programs MLTests runs, built through ML::Graph, each beside the fp32
// scalar reference it is checked against.

#include <eacp/ML/MIL/Half.h>
#include <eacp/ML/ML.h>

#include <algorithm>
#include <cmath>
#include <random>

namespace TestPrograms
{
using eacp::Vector;
using namespace eacp::ML;

inline Vector<float> seededValues(int count, unsigned seed, float spread)
{
    auto engine = std::mt19937 {seed};
    auto normal = std::normal_distribution<float> {0.0f, spread};
    auto values = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.add(halfToFloat(floatToHalf(normal(engine))));

    return values;
}

inline Vector<std::uint16_t> toHalves(const Vector<float>& values)
{
    auto halves = Vector<std::uint16_t> {};

    for (auto value: values)
        halves.add(floatToHalf(value));

    return halves;
}

inline Vector<float> zeros(int count)
{
    auto values = Vector<float> {};
    values.resize(count, 0.0f);
    return values;
}

inline Package elementwiseChain(int rows, int columns, DType type = DType::float16)
{
    auto graph = Graph {};
    auto x = graph.input("x", {rows, columns}, type);

    auto body = [](const eacp::GPU::Float& value)
    { return eacp::GPU::tanh(value * 0.5f + 0.25f); };

    graph.output(graph.apply(x, body), "y");
    return graph.build();
}

inline Vector<float> elementwiseReference(const Vector<float>& x)
{
    auto y = Vector<float> {};

    for (auto value: x)
        y.add(std::tanh(value * 0.5f + 0.25f));

    return y;
}

// softmax(x W^T + 0) over the last axis, x [rows, width], W [width, width] in
// the [out, in] order linear takes: the spike's program.
struct LinearSoftmax
{
    int rows = 0;
    int width = 0;
    Vector<float> weights;
};

inline LinearSoftmax linearSoftmaxWeights(int rows, int width, unsigned seed = 1234u)
{
    return {rows, width, seededValues(width * width, seed, 0.05f)};
}

inline Package linearSoftmax(const LinearSoftmax& net)
{
    auto graph = Graph {};
    auto x = graph.input("x", {net.rows, net.width}, DType::float16);
    auto noBias = zeros(net.width);
    auto weight = graph.halfConstant("weight", {net.width, net.width}, net.weights);
    auto bias = graph.halfConstant("bias", {net.width}, noBias);

    graph.output(graph.softmax(graph.linear(x, weight, bias), -1), "y");
    return graph.build();
}

inline void softmaxRows(Vector<float>& values, int rows, int columns)
{
    for (auto row = 0; row < rows; ++row)
    {
        auto out = values.data() + row * columns;
        auto top = -INFINITY;

        for (auto c = 0; c < columns; ++c)
            top = std::max(top, out[c]);

        auto total = 0.0;

        for (auto c = 0; c < columns; ++c)
        {
            out[c] = std::exp(out[c] - top);
            total += out[c];
        }

        for (auto c = 0; c < columns; ++c)
            out[c] = (float) (out[c] / total);
    }
}

inline Vector<float> linearSoftmaxReference(const LinearSoftmax& net,
                                            const Vector<float>& x)
{
    auto y = zeros(net.rows * net.width);

    for (auto row = 0; row < net.rows; ++row)
        for (auto o = 0; o < net.width; ++o)
        {
            auto sum = 0.0;

            for (auto i = 0; i < net.width; ++i)
                sum +=
                    (double) x[row * net.width + i] * net.weights[o * net.width + i];

            y[row * net.width + o] = (float) sum;
        }

    softmaxRows(y, net.rows, net.width);
    return y;
}

struct LayerNorm
{
    int rows = 0;
    int width = 0;
    Vector<float> gamma;
    Vector<float> beta;
    float epsilon = 1e-5f;
};

inline LayerNorm layerNormWeights(int rows, int width)
{
    auto gamma = seededValues(width, 31u, 0.2f);

    for (auto& value: gamma)
        value = halfToFloat(floatToHalf(value + 1.0f));

    return {rows, width, gamma, seededValues(width, 37u, 0.1f)};
}

inline Package layerNorm(const LayerNorm& net)
{
    auto graph = Graph {};
    auto x = graph.input("x", {net.rows, net.width}, DType::float16);
    auto gamma = graph.halfConstant("gamma", {net.width}, net.gamma);
    auto beta = graph.halfConstant("beta", {net.width}, net.beta);

    graph.output(graph.layerNorm(x, {-1}, gamma, beta, net.epsilon), "y");
    return graph.build();
}

inline Vector<float> layerNormReference(const LayerNorm& net, const Vector<float>& x)
{
    auto y = zeros(net.rows * net.width);

    for (auto row = 0; row < net.rows; ++row)
    {
        auto in = x.data() + row * net.width;
        auto mean = 0.0;

        for (auto c = 0; c < net.width; ++c)
            mean += in[c];

        mean /= net.width;

        auto variance = 0.0;

        for (auto c = 0; c < net.width; ++c)
            variance += (in[c] - mean) * (in[c] - mean);

        variance /= net.width;
        auto scale = 1.0 / std::sqrt(variance + net.epsilon);

        for (auto c = 0; c < net.width; ++c)
            y[row * net.width + c] =
                (float) ((in[c] - mean) * scale * net.gamma[c] + net.beta[c]);
    }

    return y;
}

// layer_norm(x W^T + 0), the shape of an encoder's projection followed by its
// norm: a lone layer_norm is kept on the CPU, one behind a linear is not.
inline Package linearLayerNorm(const LinearSoftmax& projection,
                               const LayerNorm& norm)
{
    auto graph = Graph {};
    auto x = graph.input("x", {norm.rows, norm.width}, DType::float16);
    auto noBias = zeros(norm.width);
    auto weight = graph.halfConstant(
        "weight", {projection.width, projection.width}, projection.weights);
    auto bias = graph.halfConstant("bias", {projection.width}, noBias);
    auto gamma = graph.halfConstant("gamma", {norm.width}, norm.gamma);
    auto beta = graph.halfConstant("beta", {norm.width}, norm.beta);

    auto projected = graph.linear(x, weight, bias);
    graph.output(graph.layerNorm(projected, {-1}, gamma, beta, norm.epsilon), "y");
    return graph.build();
}

inline Vector<float> linearLayerNormReference(const LinearSoftmax& projection,
                                              const LayerNorm& norm,
                                              const Vector<float>& x)
{
    auto projected = zeros(norm.rows * norm.width);

    for (auto row = 0; row < norm.rows; ++row)
        for (auto o = 0; o < norm.width; ++o)
        {
            auto sum = 0.0;

            for (auto i = 0; i < norm.width; ++i)
                sum += (double) x[row * norm.width + i]
                       * projection.weights[o * norm.width + i];

            projected[row * norm.width + o] = (float) sum;
        }

    return layerNormReference(norm, projected);
}

inline Package attention(int length, int depth, bool causal)
{
    auto graph = Graph {};
    auto shape = Shape {1, length, depth};
    auto q = graph.input("q", shape, DType::float16);
    auto k = graph.input("k", shape, DType::float16);
    auto v = graph.input("v", shape, DType::float16);

    graph.output(graph.scaledDotProductAttention(q, k, v, causal), "y");
    return graph.build();
}

inline Vector<float> attentionReference(const Vector<float>& q,
                                        const Vector<float>& k,
                                        const Vector<float>& v,
                                        int length,
                                        int depth,
                                        bool causal)
{
    auto scores = zeros(length * length);
    auto scale = 1.0 / std::sqrt((double) depth);

    for (auto i = 0; i < length; ++i)
        for (auto j = 0; j < length; ++j)
        {
            auto dot = 0.0;

            for (auto d = 0; d < depth; ++d)
                dot += (double) q[i * depth + d] * k[j * depth + d];

            scores[i * length + j] =
                causal && j > i ? -INFINITY : (float) (dot * scale);
        }

    softmaxRows(scores, length, length);

    auto y = zeros(length * depth);

    for (auto i = 0; i < length; ++i)
        for (auto d = 0; d < depth; ++d)
        {
            auto sum = 0.0;

            for (auto j = 0; j < length; ++j)
                sum += (double) scores[i * length + j] * v[j * depth + d];

            y[i * depth + d] = (float) sum;
        }

    return y;
}
} // namespace TestPrograms
