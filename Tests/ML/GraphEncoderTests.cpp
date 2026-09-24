#include "GraphCommon.h"

#include <cmath>

// One transformer encoder layer of the shape Whisper's has, built from every
// op at once: the combinations are where a field Core ML reads otherwise shows.

using namespace nano;
using namespace eacp;
using namespace eacp::ML;
using namespace MLGraphTesting;

using GPU::Float;

namespace
{
constexpr auto bands = 8;
constexpr auto frames = 32;
constexpr auto width = 16;
constexpr auto heads = 2;
constexpr auto headWidth = width / heads;
constexpr auto positions = frames / 2;

struct Layer
{
    Graph& graph;

    Tensor weight(std::string_view name, const Shape& shape)
    {
        auto count = static_cast<int>(shape.count());
        auto values = Vector<float> {};

        for (auto index = 0; index < count; ++index)
            values.add(0.01f * static_cast<float>((index % 7) - 3));

        return graph.halfConstant(name, shape, values);
    }

    Tensor projection(std::string_view name, Tensor x, int out, int in)
    {
        auto prefix = std::string {name};
        return graph.linear(x,
                            weight(prefix + ".weight", {out, in}),
                            weight(prefix + ".bias", {out}));
    }

    Tensor norm(std::string_view name, Tensor x)
    {
        auto prefix = std::string {name};
        return graph.layerNorm(x,
                               {-1},
                               weight(prefix + ".weight", {width}),
                               weight(prefix + ".bias", {width}));
    }

    Tensor splitHeads(Tensor rows)
    {
        return graph.transpose(graph.reshape(rows, {positions, heads, headWidth}),
                               {1, 0, 2});
    }

    Tensor mergeHeads(Tensor perHead)
    {
        return graph.reshape(graph.transpose(perHead, {1, 0, 2}),
                             {positions, width});
    }

    Tensor attention(Tensor q, Tensor k, Tensor v, bool fused)
    {
        if (fused)
            return graph.scaledDotProductAttention(q, k, v, false);

        auto scale = 1.f / std::sqrt(static_cast<float>(headWidth));
        auto scores = graph.matmul(q, k, false, true);
        auto scaled =
            graph.apply(scores, [scale](const Float& s) { return s * scale; });
        return graph.matmul(graph.softmax(scaled, -1), v);
    }
};

Graph encoderLayer(bool fused)
{
    auto graph = Graph {};
    auto layer = Layer {graph};
    auto add = [](const Float& a, const Float& b) { return a + b; };

    auto mel = graph.input("mel", {1, bands, frames}, DType::float16);
    auto conv1 =
        graph.gelu(graph.conv(mel,
                              layer.weight("conv1.weight", {width, bands, 3}),
                              layer.weight("conv1.bias", {width}),
                              1,
                              1));
    auto conv2 =
        graph.gelu(graph.conv(conv1,
                              layer.weight("conv2.weight", {width, width, 3}),
                              layer.weight("conv2.bias", {width}),
                              2,
                              1));

    auto rows = graph.reshape(graph.transpose(conv2, {0, 2, 1}), {positions, width});
    auto table = layer.weight("positional", {32, width});
    auto stream =
        graph.apply(rows, graph.slice(table, {0, 0}, {positions, width}), add);

    auto normed = layer.norm("attn_ln", stream);
    auto q = layer.splitHeads(layer.projection("q", normed, width, width));
    auto k = layer.splitHeads(layer.projection("k", normed, width, width));
    auto v = layer.splitHeads(layer.projection("v", normed, width, width));
    auto attended = layer.mergeHeads(layer.attention(q, k, v, fused));
    stream =
        graph.apply(stream, layer.projection("out", attended, width, width), add);

    auto hidden = graph.gelu(
        layer.projection("fc1", layer.norm("mlp_ln", stream), 4 * width, width));
    stream =
        graph.apply(stream, layer.projection("fc2", hidden, width, 4 * width), add);

    graph.output(graph.cast(layer.norm("ln_post", stream), DType::float32), "rows");
    return graph;
}
} // namespace

auto tEncoderUnfused = test("MLGraph/Encoder/layerWithMatmulAttention") = []
{
    auto graph = encoderLayer(false);
    buildChecked(graph);
    check(graph.specification().program.main.opset == "CoreML7");
    check(graph.specification().description.outputs[0].shape
          == Vector<std::int64_t> {positions, width});
};

auto tEncoderFused = test("MLGraph/Encoder/layerWithFusedAttention") = []
{
    auto graph = encoderLayer(true);
    buildChecked(graph);
    check(graph.specification().program.main.opset == "CoreML8");

    auto text = graph.toText();
    check(text.find("scaled_dot_product_attention(") != std::string::npos);
    check(text.find("tensor<fp32, [16, 16]> rows = cast(") != std::string::npos);
};
