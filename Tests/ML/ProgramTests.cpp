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

// Attention under an input mask, one query per head over 449 and 1500 keys,
// is kept on the CPU under every setting, with or without a projection
// either side: measured 1.2e-4 alone and 1.5e-2 projected, exactly 0 over a
// prefix of one. An ignored mask would be off by about 170. The step in
// DecoderStepTests is what takes the same op to the GPU and the engine.
constexpr auto maskedAttentionTolerance = 4e-4;
constexpr auto projectedMaskedAttentionTolerance = 5e-2;

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

constexpr auto everyUnit = Array<ComputeUnits, 4> {ComputeUnits::cpu,
                                                   ComputeUnits::cpuAndGPU,
                                                   ComputeUnits::cpuAndNeuralEngine,
                                                   ComputeUnits::all};

struct ArgmaxCase
{
    std::string name;
    Inputs inputs;
    int expected = 0;
};

MultiArray rowOf(const Vector<float>& values)
{
    return arrayOf(values, {1, (int) values.size()}, DType::float16);
}

ArgmaxCase logitsCase(const std::string& name,
                      const Vector<float>& logits,
                      const Vector<float>& suppress)
{
    auto inputs = Inputs {};
    inputs["logits"] = rowOf(logits);
    inputs["suppress"] = rowOf(suppress);
    return {name, inputs, TestPrograms::suppressedArgmaxReference(logits, suppress)};
}

Vector<float> suppressing(int vocabulary, const Vector<int>& tokens)
{
    auto suppress = TestPrograms::zeros(vocabulary);

    for (auto token: tokens)
        suppress[token] = -INFINITY;

    return suppress;
}

Vector<int> everyEvenToken(int vocabulary)
{
    auto tokens = Vector<int> {};

    for (auto token = 0; token < vocabulary; token += 2)
        tokens.add(token);

    return tokens;
}

int firstTied(int vocabulary)
{
    return vocabulary / 7;
}

int lastTied(int vocabulary)
{
    return vocabulary - 3;
}

Vector<ArgmaxCase> logitsCases(int vocabulary)
{
    auto cases = Vector<ArgmaxCase> {};
    auto none = TestPrograms::zeros(vocabulary);

    for (auto seed: {71u, 72u, 73u, 74u})
        cases.add(logitsCase("seed " + std::to_string(seed),
                             TestPrograms::seededValues(vocabulary, seed, 4.0f),
                             none));

    auto logits = TestPrograms::seededValues(vocabulary, 75u, 4.0f);
    auto top = TestPrograms::suppressedArgmaxReference(logits, none);
    cases.add(logitsCase(
        "the maximum suppressed", logits, suppressing(vocabulary, {top})));
    cases.add(logitsCase("every even token suppressed",
                         logits,
                         suppressing(vocabulary, everyEvenToken(vocabulary))));

    auto first = firstTied(vocabulary);
    auto last = lastTied(vocabulary);
    auto farApart = logits;
    farApart[first] = 100.0f;
    farApart[last] = 100.0f;
    cases.add(logitsCase("a tie far apart", farApart, none));
    cases.add(logitsCase("a tie with its first suppressed",
                         farApart,
                         suppressing(vocabulary, {first})));

    auto adjacent = logits;
    adjacent[last - 1] = 100.0f;
    adjacent[last] = 100.0f;
    cases.add(logitsCase("a tie side by side", adjacent, none));
    cases.add(logitsCase("every logit equal", none, none));

    return cases;
}

// Columns of the projection's weights set aside for the ties: a one-hot x
// reads one column out exactly on every device, so the logits are known to
// the bit even where the linear runs on the GPU or the engine.
constexpr auto farApartColumn = 1;
constexpr auto sideBySideColumn = 2;
constexpr auto equalColumn = 3;

TestPrograms::ProjectedArgmax projectedArgmaxWeights(int vocabulary, int width)
{
    auto net = TestPrograms::ProjectedArgmax {
        vocabulary,
        width,
        TestPrograms::seededValues(vocabulary * width, 81u, 4.0f)};
    auto at = [&net](int token, int column) -> float&
    { return net.weights[token * net.width + column]; };

    auto first = firstTied(vocabulary);
    auto last = lastTied(vocabulary);
    at(first, farApartColumn) = 100.0f;
    at(last, farApartColumn) = 100.0f;
    at(last - 1, sideBySideColumn) = 100.0f;
    at(last, sideBySideColumn) = 100.0f;

    for (auto token = 0; token < vocabulary; ++token)
        at(token, equalColumn) = 0.0f;

    return net;
}

ArgmaxCase projectedCase(const std::string& name,
                         const TestPrograms::ProjectedArgmax& net,
                         int column,
                         const Vector<float>& suppress)
{
    auto x = TestPrograms::zeros(net.width);
    x[column] = 1.0f;

    auto inputs = Inputs {};
    inputs["x"] = rowOf(x);
    inputs["suppress"] = rowOf(suppress);

    auto logits = TestPrograms::projectedLogits(net, x);
    return {name, inputs, TestPrograms::suppressedArgmaxReference(logits, suppress)};
}

Vector<ArgmaxCase> projectedCases(const TestPrograms::ProjectedArgmax& net)
{
    auto cases = Vector<ArgmaxCase> {};
    auto vocabulary = net.vocabulary;
    auto none = TestPrograms::zeros(vocabulary);

    for (auto column: {0, 5, 200, net.width - 1})
        cases.add(
            projectedCase("column " + std::to_string(column), net, column, none));

    auto top = projectedCase("", net, 7, none).expected;
    cases.add(projectedCase(
        "the maximum suppressed", net, 7, suppressing(vocabulary, {top})));
    cases.add(projectedCase("every even token suppressed",
                            net,
                            7,
                            suppressing(vocabulary, everyEvenToken(vocabulary))));

    cases.add(projectedCase("a tie far apart", net, farApartColumn, none));
    cases.add(projectedCase("a tie with its first suppressed",
                            net,
                            farApartColumn,
                            suppressing(vocabulary, {firstTied(vocabulary)})));
    cases.add(projectedCase("a tie side by side", net, sideBySideColumn, none));
    cases.add(projectedCase("every logit equal", net, equalColumn, none));

    return cases;
}

void checkArgmax(const std::string& what,
                 const Package& package,
                 const Vector<ArgmaxCase>& cases)
{
    check(!package.isEmpty());

    auto cache = freshCacheDirectory(what);

    for (auto units: everyUnit)
    {
        auto model = Model {};
        auto loaded = model.load(package, optionsFor(units, cache));
        check(loaded.ok, loaded.error);

        if (!loaded)
            return;

        for (const auto& entry: cases)
        {
            auto outputs = Outputs {};
            outputs["token"] = MultiArray::create({1}, DType::int32);

            auto result = model.predict(entry.inputs, outputs);
            check(result.ok, result.error);

            auto token = (int) outputs["token"].toFloats()[0];
            check(token == entry.expected,
                  what + ", " + entry.name + ", under " + nameOf(units) + ": token "
                      + std::to_string(token) + " where the reference is "
                      + std::to_string(entry.expected));
        }

        LOG(what, " [", nameOf(units), "] placed ", placementOf(model));
    }
}

// The rows past the prefix replaced by values fifty times larger, so an
// attention that ignored the mask would be far from the reference.
Vector<float> beyondThePrefix(const TestPrograms::MaskedAttention& net,
                              const Vector<float>& values,
                              const Vector<float>& noise,
                              int prefix)
{
    auto result = values;

    for (auto head = 0; head < net.heads; ++head)
        for (auto j = prefix; j < net.keys; ++j)
            for (auto d = 0; d < net.depth; ++d)
            {
                auto index = (head * net.keys + j) * net.depth + d;
                result[index] = noise[index];
            }

    return result;
}

void checkMaskedAttention(int keys, bool projected, const Tolerances& tolerances)
{
    if (!isSupportedAttention())
        return;

    auto net = projected ? TestPrograms::projectedMaskedAttention(6, keys, 64)
                         : TestPrograms::MaskedAttention {6, keys, 64};
    auto cacheCount = net.heads * net.keys * net.depth;
    auto label = std::string {projected ? "projected " : ""} + "masked attention";
    auto cache = freshCacheDirectory(
        (projected ? "projected-masked-attention-" : "masked-attention-")
        + std::to_string(keys));
    auto package = TestPrograms::maskedAttention(net);
    check(!package.isEmpty());

    auto q = TestPrograms::seededValues(net.heads * net.depth, 61u, 1.0f);
    auto k = TestPrograms::seededValues(cacheCount, 62u, 1.0f);
    auto v = TestPrograms::seededValues(cacheCount, 63u, 1.0f);
    auto keyNoise = TestPrograms::seededValues(cacheCount, 64u, 50.0f);
    auto valueNoise = TestPrograms::seededValues(cacheCount, 65u, 50.0f);

    for (const auto& tolerance: tolerances)
    {
        auto model = Model {};
        auto loaded = model.load(package, optionsFor(tolerance.units, cache));
        check(loaded.ok, loaded.error);

        if (!loaded)
            return;

        for (auto prefix: Vector<int> {1, keys * 2 / 5, keys})
        {
            auto maskedK = beyondThePrefix(net, k, keyNoise, prefix);
            auto maskedV = beyondThePrefix(net, v, valueNoise, prefix);
            auto allowed = TestPrograms::prefixMask(keys, prefix);

            auto inputs = Inputs {};
            inputs["q"] = arrayOf(q, net.queryShape(), DType::float16);
            inputs["k"] = arrayOf(maskedK, net.cacheShape(), DType::float16);
            inputs["v"] = arrayOf(maskedV, net.cacheShape(), DType::float16);
            inputs["allowed"] = arrayOf(allowed, net.maskShape(), DType::float16);

            auto outputs = Outputs {};
            outputs["y"] = MultiArray::create(net.queryShape(), DType::float16);

            auto result = model.predict(inputs, outputs);
            check(result.ok, result.error);

            auto actual = outputs["y"].toFloats();
            auto expected = TestPrograms::prefixAttentionReference(
                net, q, maskedK, maskedV, prefix);
            auto what = label + " over " + std::to_string(prefix) + " of "
                        + std::to_string(keys);

            checkWithin(what,
                        toleranceWhereItRan(tolerances, tolerance, model),
                        model,
                        compare(actual, expected));

            if (prefix == keys)
                continue;

            auto unmasked = TestPrograms::prefixAttentionReference(
                net, q, maskedK, maskedV, keys);
            check(compare(actual, unmasked).maxAbs > 1.0,
                  what + " is as close to the unmasked attention");
        }
    }
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

// A tie goes to the lowest index, measured on the CPU and, behind the
// projection below, on the GPU; no setting puts reduce_argmax on the engine,
// which hands it to the CPU. A suppressing -inf is exact on all three.
auto tArgmaxMatchesReference =
    test("MLPrograms/aSuppressedArgmaxPicksTheReferenceTokenOnEveryDevice") = []
{
    if (!isSupported())
        return;

    for (auto vocabulary: {64, 51864})
        checkArgmax("argmax over " + std::to_string(vocabulary),
                    TestPrograms::suppressedArgmax(vocabulary),
                    logitsCases(vocabulary));
};

// Whisper's logits projection, 51864 tokens of 384, in front of the argmax:
// a lone argmax stays on the CPU, and this is what moves it.
auto tProjectedArgmaxMatchesReference =
    test("MLPrograms/anArgmaxBehindTheLogitsProjectionOnEveryDevice") = []
{
    if (!isSupported())
        return;

    auto net = projectedArgmaxWeights(51864, 384);
    checkArgmax(
        "projected argmax", TestPrograms::projectedArgmax(net), projectedCases(net));
};

auto tMaskedAttention =
    test("MLPrograms/anInputMaskHoldsAttentionToAPrefixOnEveryDevice") = []
{
    for (auto keys: {449, 1500})
        checkMaskedAttention(keys, false, sameOnEveryUnit(maskedAttentionTolerance));
};

auto tProjectedMaskedAttention =
    test("MLPrograms/anInputMaskHoldsAProjectedAttentionToAPrefixOnEveryDevice") = []
{
    for (auto keys: {449, 1500})
        checkMaskedAttention(
            keys, true, sameOnEveryUnit(projectedMaskedAttentionTolerance));
};

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
