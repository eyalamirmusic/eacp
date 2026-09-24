#include "GraphCommon.h"

// Where Core ML is here, a few lowerings are run as well as compiled: the ones
// whose meaning a successful compile does not pin down. MLTests measures the
// device tolerances; these run on the CPU, where the answer is exact enough to
// tell a right lowering from a wrong one.

using namespace nano;
using namespace eacp;
using namespace eacp::ML;
using namespace MLGraphTesting;

using GPU::Float;

#if EACP_HAS_COREML
#include <algorithm>
#include <cmath>

using MLSuiteCommon::arrayOf;

namespace
{
Result runOnCpu(Model& model,
                const Graph& graph,
                const Inputs& inputs,
                Outputs& outputs)
{
    auto options = Options {};
    options.units = ComputeUnits::cpu;
    options.cacheDirectory = coreMLCacheDirectory();

    auto loaded = model.load(buildChecked(graph), options);

    if (!loaded.ok)
        return loaded;

    return model.predict(inputs, outputs);
}
} // namespace

auto tCoreMLApply = test("MLGraph/CoreML/applyComputesTheLoweredExpression") = []
{
    if (!isSupported())
        return;

    auto graph = Graph {};
    auto x = graph.input("x", {6}, DType::float32);
    auto body = [](const Float& value)
    { return select(value > 0.f, clamp(value * 2.f, 0.f, 3.f), exp(value) - 1.f); };
    graph.output(graph.apply(x, body), "y");

    auto values = Vector<float> {-2.f, -0.5f, 0.f, 0.25f, 1.f, 4.f};
    auto inputs = Inputs {};
    inputs["x"] = arrayOf(values, {6}, DType::float32);

    auto model = Model {};
    auto outputs = Outputs {};
    auto result = runOnCpu(model, graph, inputs, outputs);
    check(result.ok, result.error);

    if (!result.ok)
        return;

    auto results = outputs["y"].toFloats();
    check(results.size() == values.size());

    for (auto index = 0; index < values.size() && index < results.size(); ++index)
    {
        auto value = values[index];
        auto expected =
            value > 0.f ? std::clamp(value * 2.f, 0.f, 3.f) : std::exp(value) - 1.f;
        check(std::abs(results[index] - expected) < 1e-5f);
    }
};

// Core ML's fp16 scaled_dot_product_attention ignores an additive float mask
// once the sequence is 32 or longer - the output is the unmasked one, on every
// compute unit - and honours a bool mask at every length. With q and k zero
// every score ties, so row i is the mean of the values of rows 0..i.
auto tCoreMLCausal = test("MLGraph/CoreML/causalAttentionMasksInHalfPrecision") = []
{
    if (!isSupported())
        return;

    constexpr auto length = 64;
    constexpr auto depth = 8;
    auto shape = Shape {1, length, depth};

    auto graph = Graph {};
    auto q = graph.input("q", shape, DType::float16);
    auto k = graph.input("k", shape, DType::float16);
    auto v = graph.input("v", shape, DType::float16);
    graph.output(graph.scaledDotProductAttention(q, k, v, true), "y");

    auto zeros = Vector<float> {};
    zeros.resize(length * depth, 0.f);

    auto rowIndices = Vector<float> {};

    for (auto row = 0; row < length; ++row)
        for (auto column = 0; column < depth; ++column)
            rowIndices.add(static_cast<float>(row));

    auto inputs = Inputs {};
    inputs["q"] = arrayOf(zeros, shape, DType::float16);
    inputs["k"] = arrayOf(zeros, shape, DType::float16);
    inputs["v"] = arrayOf(rowIndices, shape, DType::float16);

    auto model = Model {};
    auto outputs = Outputs {};
    auto result = runOnCpu(model, graph, inputs, outputs);
    check(result.ok, result.error);

    if (!result.ok)
        return;

    auto results = outputs["y"].toFloats();
    check(results.size() == length * depth);

    for (auto row = 0; row < length && row * depth < results.size(); ++row)
    {
        auto expected = static_cast<float>(row) / 2.f;
        auto error = std::abs(results[row * depth] - expected);
        check(error <= 0.01f + 0.005f * expected,
              "row " + std::to_string(row) + " is "
                  + std::to_string(results[row * depth]));
    }
};
#endif
