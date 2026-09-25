#include "ModelTestCommon.h"

using namespace nano;
using namespace ModelTests;

// Small programs run under every compute-unit setting and checked against an
// fp32 scalar reference. Each setting has its own tolerance because each
// device has its own fp16 arithmetic: the spike found the CPU's fp16 path the
// least accurate of the three, so a bound measured on the engine would fail a
// CPU run and one measured on the CPU would hide an engine regression.
namespace
{
struct Tolerance
{
    ComputeUnits units;
    double maxAbs;
};

using Tolerances = Array<Tolerance, 4>;

Tolerances sameOnEveryUnit(double maxAbs)
{
    return {{ComputeUnits::cpu, maxAbs},
            {ComputeUnits::cpuAndGPU, maxAbs},
            {ComputeUnits::cpuAndNeuralEngine, maxAbs},
            {ComputeUnits::all, maxAbs}};
}

// Core ML keeps an elementwise-only program on the CPU under every setting,
// measured at 9.5e-4 (a unit in the last fp16 place of tanh near 1).
constexpr auto elementwiseTolerance = 2e-3;

// About three times what was measured: CPU 1.4e-3, GPU 1.9e-4, Neural Engine
// 3.1e-4.
const auto linearSoftmaxTolerances = Tolerances {
    {ComputeUnits::cpu, 4e-3},
    {ComputeUnits::cpuAndGPU, 6e-4},
    {ComputeUnits::cpuAndNeuralEngine, 1e-3},
    {ComputeUnits::all, 1e-3},
};

// A lone layer_norm or scaled_dot_product_attention is kept on the CPU under
// every setting too; measured 3.1e-3 and 4.8e-3.
constexpr auto layerNormTolerance = 1e-2;
constexpr auto attentionTolerance = 1.5e-2;

// Behind a linear the norm moves to the GPU and the engine, and the CPU's fp16
// projection error comes out of the norm magnified: measured CPU 3.7e-2, GPU
// 5.0e-3, Neural Engine 9.4e-3.
const auto linearLayerNormTolerances = Tolerances {
    {ComputeUnits::cpu, 5e-2},
    {ComputeUnits::cpuAndGPU, 1.5e-2},
    {ComputeUnits::cpuAndNeuralEngine, 3e-2},
    {ComputeUnits::all, 3e-2},
};

// scaled_dot_product_attention is an iOS 18 op: an ML Program holding one
// needs the CoreML8 opset, which macOS 15 / iOS 18 are the first to load.
bool isSupportedAttention()
{
    return isSupported() && osVersion().atLeast(isIOS() ? 18 : 15, 0);
}

bool isHeldToTheEngine(ComputeUnits units)
{
    return isAneRequired()
           && (units == ComputeUnits::cpuAndNeuralEngine
               || units == ComputeUnits::all);
}

bool ranOnTheCpu(const Model& model)
{
    auto plan = model.computePlan();
    return plan.isEmpty() || plan.allOn(ComputePlan::Device::cpu);
}

double cpuBoundOf(const Tolerances& tolerances)
{
    auto isCpu = [](const Tolerance& entry)
    { return entry.units == ComputeUnits::cpu; };
    return tolerances.findIf(isCpu)->maxAbs;
}

Tolerance toleranceWhereItRan(const Tolerances& tolerances,
                              const Tolerance& requested,
                              const Model& model)
{
    if (isHeldToTheEngine(requested.units) || !ranOnTheCpu(model))
        return requested;

    return {requested.units, cpuBoundOf(tolerances)};
}

void checkWithin(const std::string& what,
                 const Tolerance& tolerance,
                 const Model& model,
                 const Errors& errors)
{
    LOG(what,
        " [",
        nameOf(tolerance.units),
        "] maxAbs ",
        errors.maxAbs,
        " maxRel ",
        errors.maxRel,
        " placed ",
        placementOf(model));

    check(errors.maxAbs <= tolerance.maxAbs,
          what + " under " + nameOf(tolerance.units) + ": max abs error "
              + std::to_string(errors.maxAbs) + " exceeds "
              + std::to_string(tolerance.maxAbs));
}

void checkAttention(bool causal)
{
    if (!isSupportedAttention())
        return;

    constexpr auto length = 128;
    constexpr auto depth = 64;

    auto shape = Shape {1, length, depth};
    auto q = TestPrograms::seededValues(length * depth, 51u, 1.0f);
    auto k = TestPrograms::seededValues(length * depth, 52u, 1.0f);
    auto v = TestPrograms::seededValues(length * depth, 53u, 1.0f);

    auto what = std::string {causal ? "causal attention" : "attention"};
    auto cache = freshCacheDirectory(causal ? "causal-attention" : "attention");
    auto package = TestPrograms::attention(length, depth, causal);
    check(!package.isEmpty());

    auto expected = TestPrograms::attentionReference(q, k, v, length, depth, causal);

    for (const auto& tolerance: sameOnEveryUnit(attentionTolerance))
    {
        auto model = Model {};
        auto loaded = model.load(package, optionsFor(tolerance.units, cache));
        check(loaded.ok, loaded.error);

        if (!loaded)
            return;

        auto inputs = Inputs {};
        inputs["q"] = arrayOf(q, shape, DType::float16);
        inputs["k"] = arrayOf(k, shape, DType::float16);
        inputs["v"] = arrayOf(v, shape, DType::float16);

        auto outputs = Outputs {};
        outputs["y"] = MultiArray::create(shape, DType::float16);

        auto result = model.predict(inputs, outputs);
        check(result.ok, result.error);

        checkWithin(
            what, tolerance, model, compare(outputs["y"].toFloats(), expected));
    }
}

Vector<float> predictOnce(Model& model, const Shape& shape, const Vector<float>& x)
{
    auto inputs = Inputs {};
    inputs["x"] = arrayOf(x, shape, DType::float16);

    auto outputs = Outputs {};
    outputs["y"] = MultiArray::create(shape, DType::float16);

    auto result = model.predict(inputs, outputs);
    check(result.ok, result.error);
    return outputs["y"].toFloats();
}
} // namespace

auto tElementwiseChainMatchesReference =
    test("MLPrograms/anElementwiseChainMatchesTheReferenceOnEveryDevice") = []
{
    if (!isSupported())
        return;

    constexpr auto rows = 1500;
    constexpr auto columns = 384;

    auto cache = freshCacheDirectory("elementwise");
    auto package = TestPrograms::elementwiseChain(rows, columns);
    auto x = TestPrograms::seededValues(rows * columns, 7u, 2.0f);
    auto expected = TestPrograms::elementwiseReference(x);

    for (const auto& tolerance: sameOnEveryUnit(elementwiseTolerance))
    {
        auto model = Model {};
        auto loaded = model.load(package, optionsFor(tolerance.units, cache));
        check(loaded.ok, loaded.error);

        if (!loaded)
            return;

        auto actual = predictOnce(model, {rows, columns}, x);
        checkWithin("elementwise", tolerance, model, compare(actual, expected));
    }
};

auto tLinearSoftmaxMatchesReference =
    test("MLPrograms/aLinearAndSoftmaxMatchTheReferenceOnEveryDevice") = []
{
    if (!isSupported())
        return;

    auto net = TestPrograms::linearSoftmaxWeights(1500, 384);
    auto cache = freshCacheDirectory("linear-softmax");
    auto package = TestPrograms::linearSoftmax(net);
    auto x = TestPrograms::seededValues(net.rows * net.width, 42u, 1.0f);
    auto expected = TestPrograms::linearSoftmaxReference(net, x);

    for (const auto& tolerance: linearSoftmaxTolerances)
    {
        auto model = Model {};
        auto loaded = model.load(package, optionsFor(tolerance.units, cache));
        check(loaded.ok, loaded.error);

        if (!loaded)
            return;

        auto actual = predictOnce(model, {net.rows, net.width}, x);
        checkWithin("linear+softmax",
                    toleranceWhereItRan(linearSoftmaxTolerances, tolerance, model),
                    model,
                    compare(actual, expected));
    }
};

auto tLayerNormMatchesReference =
    test("MLPrograms/aLayerNormMatchesTheReferenceOnEveryDevice") = []
{
    if (!isSupported())
        return;

    auto net = TestPrograms::layerNormWeights(1500, 384);
    auto cache = freshCacheDirectory("layer-norm");
    auto package = TestPrograms::layerNorm(net);
    check(!package.isEmpty());

    auto x = TestPrograms::seededValues(net.rows * net.width, 43u, 1.5f);
    auto expected = TestPrograms::layerNormReference(net, x);

    for (const auto& tolerance: sameOnEveryUnit(layerNormTolerance))
    {
        auto model = Model {};
        auto loaded = model.load(package, optionsFor(tolerance.units, cache));
        check(loaded.ok, loaded.error);

        if (!loaded)
            return;

        auto actual = predictOnce(model, {net.rows, net.width}, x);
        checkWithin("layer norm", tolerance, model, compare(actual, expected));
    }
};

auto tLinearLayerNormMatchesReference =
    test("MLPrograms/aLinearIntoALayerNormMatchesTheReferenceOnEveryDevice") = []
{
    if (!isSupported())
        return;

    auto projection = TestPrograms::linearSoftmaxWeights(1500, 384, 99u);
    auto norm = TestPrograms::layerNormWeights(1500, 384);
    auto cache = freshCacheDirectory("linear-layer-norm");
    auto package = TestPrograms::linearLayerNorm(projection, norm);
    check(!package.isEmpty());

    auto x = TestPrograms::seededValues(norm.rows * norm.width, 44u, 1.0f);
    auto expected = TestPrograms::linearLayerNormReference(projection, norm, x);

    for (const auto& tolerance: linearLayerNormTolerances)
    {
        auto model = Model {};
        auto loaded = model.load(package, optionsFor(tolerance.units, cache));
        check(loaded.ok, loaded.error);

        if (!loaded)
            return;

        auto actual = predictOnce(model, {norm.rows, norm.width}, x);
        checkWithin("linear+layer norm",
                    toleranceWhereItRan(linearLayerNormTolerances, tolerance, model),
                    model,
                    compare(actual, expected));
    }
};

auto tAttentionMatchesReference =
    test("MLPrograms/anAttentionBlockMatchesTheReferenceOnEveryDevice") = []
{ checkAttention(false); };

auto tCausalAttentionMatchesReference =
    test("MLPrograms/aCausalAttentionBlockMatchesTheReferenceOnEveryDevice") = []
{ checkAttention(true); };

auto tFloat32ProgramRunsOnPlainArrays =
    test("MLPrograms/anFp32ProgramRunsOnPlainArrays") = []
{
    if (!isSupported())
        return;

    constexpr auto rows = 8;
    constexpr auto columns = 16;

    auto package = TestPrograms::elementwiseChain(rows, columns, DType::float32);
    auto model = Model {};
    auto loaded = model.load(
        package, optionsFor(ComputeUnits::cpu, freshCacheDirectory("fp32")));
    check(loaded.ok, loaded.error);

    if (!loaded)
        return;

    auto x = TestPrograms::seededValues(rows * columns, 3u, 1.0f);
    auto inputs = Inputs {};
    inputs["x"] = arrayOf(x, {rows, columns}, DType::float32);
    check(!inputs["x"].isSurfaceBacked());

    auto outputs = Outputs {};
    outputs["y"] = MultiArray::create({rows, columns}, DType::float32);

    auto result = model.predict(inputs, outputs);
    check(result.ok, result.error);

    auto errors =
        compare(outputs["y"].toFloats(), TestPrograms::elementwiseReference(x));
    check(errors.maxAbs <= 1e-5);
};

auto tUnboundOutputsAreAllocated =
    test("MLPrograms/anOutputNobodyBoundIsAllocatedAndAdded") = []
{
    if (!isSupported())
        return;

    constexpr auto rows = 16;
    constexpr auto columns = 32;

    auto model = Model {};
    auto loaded =
        model.load(TestPrograms::elementwiseChain(rows, columns),
                   optionsFor(ComputeUnits::all, freshCacheDirectory("unbound")));
    check(loaded.ok, loaded.error);

    if (!loaded)
        return;

    auto x = TestPrograms::seededValues(rows * columns, 5u, 1.0f);
    auto inputs = Inputs {};
    inputs["x"] = arrayOf(x, {rows, columns}, DType::float16);

    auto outputs = Outputs {};
    auto result = model.predict(inputs, outputs);
    check(result.ok, result.error);

    auto y = outputs.getValue("y");
    check(y != nullptr && y->isValid());

    if (y == nullptr || !y->isValid())
        return;

    check(y->shape() == Shape {rows, columns});
    check(compare(y->toFloats(), TestPrograms::elementwiseReference(x)).maxAbs
          <= 2e-3);
};

auto tDescriptionNamesTheFeatures =
    test("MLPrograms/theDescriptionNamesTheGraphsFeatures") = []
{
    if (!isSupported())
        return;

    auto model = Model {};
    auto loaded =
        model.load(TestPrograms::elementwiseChain(4, 8),
                   optionsFor(ComputeUnits::cpu, freshCacheDirectory("describe")));
    check(loaded.ok, loaded.error);

    auto inputs = model.inputs();
    auto outputs = model.outputs();

    check(inputs.size() == 1 && outputs.size() == 1);

    if (inputs.size() != 1 || outputs.size() != 1)
        return;

    check(inputs[0].name == "x");
    check(inputs[0].shape == Shape {4, 8});
    check(inputs[0].type == DType::float16, toString(inputs[0].type));
    check(inputs[0].enumeratedShapes.empty());
    check(outputs[0].name == "y");
    check(outputs[0].type == DType::float16);
};

auto tMissingInputFails = test("MLPrograms/aMissingInputFailsWithAMessage") = []
{
    if (!isSupported())
        return;

    auto model = Model {};
    auto loaded =
        model.load(TestPrograms::elementwiseChain(4, 8),
                   optionsFor(ComputeUnits::cpu, freshCacheDirectory("missing")));
    check(loaded.ok, loaded.error);

    auto outputs = Outputs {};
    auto result = model.predict({}, outputs);
    check(!result.ok);
    check(result.error.find("input x") != std::string::npos, result.error);
};

auto tUnloadedModelFails = test("MLPrograms/anUnloadedModelRefusesToPredict") = []
{
    auto model = Model {};
    auto outputs = Outputs {};

    check(!model.isLoaded());
    check(!model.predict({}, outputs).ok);
    check(model.computePlan().isEmpty());
};
