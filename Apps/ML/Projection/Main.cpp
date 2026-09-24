#include <eacp/ML/ML.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <random>

using namespace eacp;

namespace
{
constexpr auto width = 384;
constexpr auto warmRuns = 3;
constexpr auto timedRuns = 20;

struct UnitsChoice
{
    const char* name;
    ML::ComputeUnits units;
};

constexpr auto unitChoices = Array<UnitsChoice, 4> {
    {"all", ML::ComputeUnits::all},
    {"cpu-ane", ML::ComputeUnits::cpuAndNeuralEngine},
    {"cpu-gpu", ML::ComputeUnits::cpuAndGPU},
    {"cpu", ML::ComputeUnits::cpu},
};

std::optional<ML::ComputeUnits> unitsNamed(const std::string& name)
{
    for (const auto& choice: unitChoices)
        if (name == choice.name)
            return choice.units;

    return std::nullopt;
}

const char* nameOf(ML::ComputeUnits units)
{
    for (const auto& choice: unitChoices)
        if (units == choice.units)
            return choice.name;

    return "?";
}

std::optional<ML::ComputeUnits> parseUnits(int argc, char** argv)
{
    auto units = ML::ComputeUnits::all;

    for (auto i = 1; i < argc; ++i)
    {
        auto arg = std::string {argv[i]};

        if (arg != "--units" || i + 1 >= argc)
            return std::nullopt;

        auto named = unitsNamed(argv[++i]);

        if (!named)
            return std::nullopt;

        units = *named;
    }

    return units;
}

const char* yesNo(bool value)
{
    return value ? "yes" : "no";
}

Vector<float> seededHalves(int count, unsigned seed, float spread)
{
    auto engine = std::mt19937 {seed};
    auto normal = std::normal_distribution<float> {0.0f, spread};
    auto values = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.add(ML::halfToFloat(ML::floatToHalf(normal(engine))));

    return values;
}

ML::Package projectionAndSoftmax(int rows, const Vector<float>& weights)
{
    auto zeros = Vector<float> {};
    zeros.resize(width, 0.0f);

    auto graph = ML::Graph {};
    auto x = graph.input("x", {rows, width}, ML::DType::float16);
    auto weight = graph.halfConstant("weight", {width, width}, weights);
    auto bias = graph.halfConstant("bias", {width}, zeros);

    graph.output(graph.softmax(graph.linear(x, weight, bias), -1), "y");

    if (!graph.isValid())
        std::printf("graph error: %s\n", graph.errors()[0].c_str());

    return graph.build();
}

void printPlan(const ML::Model& model)
{
    if (!ML::hasComputePlan())
    {
        std::printf("  compute plan: not available on this OS\n");
        return;
    }

    auto plan = model.computePlan();

    if (plan.isEmpty())
        std::printf("  compute plan: empty\n");

    for (const auto& op: plan.ops)
        std::printf("  %-24s %-10s %-14s cost %.3f\n",
                    op.name.c_str(),
                    op.type.c_str(),
                    ML::toString(op.device).c_str(),
                    op.cost);
}

double millisecondsOf(const std::function<void()>& work)
{
    using Clock = std::chrono::steady_clock;

    auto start = Clock::now();
    work();
    auto elapsed = Clock::now() - start;

    return std::chrono::duration<double, std::milli>(elapsed).count();
}

double median(Vector<double> values)
{
    values.sort();
    return values[values.size() / 2];
}

bool timePrediction(ML::Model& model, int rows)
{
    auto input = ML::MultiArray::create({rows, width}, ML::DType::float16);
    auto output = ML::MultiArray::create({rows, width}, ML::DType::float16);
    auto values = seededHalves(rows * width, 42u + (unsigned) rows, 1.0f);
    input.fromFloats(values);

    auto inputs = ML::Inputs {};
    inputs["x"] = input;
    auto outputs = ML::Outputs {};
    outputs["y"] = output;

    auto result = ML::Result::success();
    auto predictOnce = [&] { result = model.predict(inputs, outputs); };

    for (auto i = 0; i < warmRuns && result; ++i)
        predictOnce();

    auto times = Vector<double> {};

    for (auto i = 0; i < timedRuns && result; ++i)
        times.add(millisecondsOf(predictOnce));

    if (!result)
    {
        std::printf("  predict failed: %s\n", result.error.c_str());
        return false;
    }

    std::printf("  predict median %.3f ms over %d runs (%d warm)\n",
                median(times),
                timedRuns,
                warmRuns);
    return true;
}

bool runSize(int rows, const Vector<float>& weights, ML::ComputeUnits units)
{
    std::printf("\n[%d, %d] x [%d, %d] -> softmax\n", rows, width, width, width);

    auto package = projectionAndSoftmax(rows, weights);

    if (package.isEmpty())
        return false;

    auto options = ML::Options {};
    options.units = units;

    auto model = ML::Model {};
    auto result = ML::Result::failure("not loaded");
    auto loadOnce = [&] { result = model.load(package, options); };
    auto loadTime = millisecondsOf(loadOnce);

    if (!result)
    {
        std::printf("  load failed: %s\n", result.error.c_str());
        return false;
    }

    std::printf("  load %.1f ms, cache %s\n  compiled at %s\n",
                loadTime,
                model.wasCacheHit() ? "hit" : "miss (compiled)",
                model.compiledPath().str().c_str());

    printPlan(model);
    return timePrediction(model, rows);
}
} // namespace

int main(int argc, char** argv)
{
    auto units = parseUnits(argc, argv);

    if (!units)
    {
        std::printf("usage: MLProjection [--units all|cpu-ane|cpu-gpu|cpu]\n");
        return 1;
    }

    std::printf("Core ML supported: %s\ncompute plan: %s\nNeural Engine: %s\n",
                yesNo(ML::isSupported()),
                yesNo(ML::hasComputePlan()),
                yesNo(ML::hasNeuralEngine()));

    if (!ML::isSupported())
    {
        std::printf("This OS has no Core ML runtime eacp can use "
                    "(macOS 13 or later is needed).\n");
        return 0;
    }

    std::printf("compute units: %s\n", nameOf(*units));

    auto weights = seededHalves(width * width, 1234u, 0.05f);
    auto allRan = true;

    for (auto rows: {448, 1024, 1500})
        allRan = runSize(rows, weights, *units) && allRan;

    return allRan ? 0 : 1;
}
