#include "EncoderReference.h"
#include "ModelTestCommon.h"

#include <chrono>

using namespace nano;
using namespace ModelTests;

// The Whisper tiny.en encoder at its real sizes, enumerated over the eighteen
// audio contexts, run on every compute-unit setting against the fp32
// reference, with its placement read back and its load and prediction times
// logged. The same encoder fixed at one context is the probe for where a
// member goes, since a compute plan is read from the compiled model and takes
// no shape.
namespace
{
using Clock = std::chrono::steady_clock;
using WhisperEncoder::melShape;

constexpr auto width = WhisperEncoder::width;

double millisecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(Vector<double> values)
{
    std::sort(values.begin(), values.end());
    return values.empty() ? 0.0 : values[values.size() / 2];
}

struct Member
{
    int context = 0;
    Vector<float> mel;
    Vector<float> expected;
};

const Vector<Member>& checkedMembers()
{
    static const auto members = []
    {
        auto window = WhisperEncoder::melWindow();
        auto result = Vector<Member> {};

        for (auto context: {1500, 448, 576})
        {
            auto member = Member {};
            member.context = context;
            member.mel = WhisperEncoder::melFor(window, context);

            auto start = Clock::now();
            member.expected = WhisperEncoder::referenceEncoder(
                WhisperEncoder::sharedWeights(), member.mel, context);
            LOG("encoder reference at ",
                context,
                ": ",
                millisecondsSince(start),
                " ms");
            result.add(member);
        }

        return result;
    }();

    return members;
}

const Member& memberAt(int context)
{
    auto isContext = [context](const Member& member)
    { return member.context == context; };
    return *checkedMembers().findIf(isContext);
}

const Package& enumeratedPackage()
{
    static const auto package = WhisperEncoder::enumeratedEncoderGraph().build();
    return package;
}

// One cache for the suite, so the engine compiles the enumerated model once
// rather than once per test.
const FilePath& encoderCache()
{
    static const auto cache = freshCacheDirectory("whisper-encoder");
    return cache;
}

bool isSupportedEncoder()
{
    return supportsSpecification(9);
}

struct Encoded
{
    Vector<float> rows;
    double milliseconds = 0.0;
};

Encoded encode(Model& model, const Member& member)
{
    auto inputs = Inputs {};
    inputs["mel"] = arrayOf(member.mel, melShape(member.context), DType::float16);

    auto outputs = Outputs {};
    outputs["rows"] = MultiArray::create({member.context, width}, DType::float16);

    auto start = Clock::now();
    auto result = model.predict(inputs, outputs);
    auto elapsed = millisecondsSince(start);
    check(result.ok, result.error);

    return {outputs["rows"].toFloats(), elapsed};
}

double timedLoad(Model& model,
                 const Package& package,
                 ComputeUnits units,
                 const FilePath& cache)
{
    auto start = Clock::now();
    auto loaded = model.load(package, optionsFor(units, cache));
    auto elapsed = millisecondsSince(start);
    check(loaded.ok, loaded.error);
    return elapsed;
}

double medianPrediction(Model& model, const Member& member, int runs)
{
    auto times = Vector<double> {};

    for (auto run = 0; run < runs; ++run)
        times.add(encode(model, member).milliseconds);

    return median(times);
}

bool isEncoderOp(const std::string& type)
{
    return type == "linear" || type == "conv" || type == "layer_norm"
           || type == "scaled_dot_product_attention";
}

// Each op type with the device its ops went to: "linear NeuralEngine x24".
std::string placementSummary(const ComputePlan& plan)
{
    auto counts = EA::MapVector<std::string, int> {};

    for (const auto& op: plan.ops)
        counts[op.type + " " + toString(op.device)] += 1;

    auto text = std::string {};

    for (const auto& entry: counts)
        text += (text.empty() ? "" : ", ") + entry.first + " x"
                + std::to_string(entry.second);

    return text.empty() ? "(no plan)" : text;
}

Vector<std::string> offTheEngine(const ComputePlan& plan)
{
    auto misplaced = Vector<std::string> {};

    for (const auto& op: plan.ops)
        if (isEncoderOp(op.type) && op.device != ComputePlan::Device::neuralEngine)
            misplaced.add(op.type + " " + op.name + " on " + toString(op.device));

    return misplaced;
}

void checkOnTheEngine(const std::string& what, const ComputePlan& plan)
{
    LOG(what, " placed: ", placementSummary(plan));

    if (plan.isEmpty())
    {
        check(!isAneRequired(), what + ": no compute plan to read");
        return;
    }

    auto misplaced = offTheEngine(plan);

    if (misplaced.empty())
        return;

    if (!isAneRequired())
    {
        LOG("skipped: ", what, " has ", misplaced.size(), " ops off the engine");
        return;
    }

    check(false,
          what + ": " + misplaced[0] + " and " + std::to_string(misplaced.size() - 1)
              + " more off the Neural Engine");
}

bool canCheckPlacement()
{
    if (isSupportedEncoder() && hasComputePlan() && hasNeuralEngine())
        return true;

    check(!isAneRequired(), "no CoreML8, compute plan or engine to place on");
    return false;
}

struct Tolerance
{
    ComputeUnits units;
    double maxAbs;
};

// About three times what was measured over 1500, 448 and 576 rows: CPU
// 4.9e-2, GPU 6.4e-3, Neural Engine 2.3e-2.
constexpr auto cpuTolerance = 0.15;

const auto encoderTolerances = Array<Tolerance, 4> {{
    {ComputeUnits::cpu, cpuTolerance},
    {ComputeUnits::cpuAndGPU, 0.02},
    {ComputeUnits::cpuAndNeuralEngine, 0.07},
    {ComputeUnits::all, 0.07},
}};

bool isEngineSetting(ComputeUnits units)
{
    return units == ComputeUnits::cpuAndNeuralEngine || units == ComputeUnits::all;
}

// A setting that stayed on the CPU is held to the CPU's bound. The engine
// settings keep their own only under EACP_REQUIRE_ANE=1, because reading the
// plan of an engine compile costs as long as the compile; the GPU setting keeps
// its own where its plan shows it left the CPU.
double boundFor(const Tolerance& tolerance, const Model& model)
{
    if (isEngineSetting(tolerance.units))
        return isAneRequired() ? tolerance.maxAbs : cpuTolerance;

    if (tolerance.units != ComputeUnits::cpuAndGPU)
        return tolerance.maxAbs;

    auto plan = model.computePlan();
    auto stayedOnTheCpu = plan.isEmpty() || plan.allOn(ComputePlan::Device::cpu);
    return stayedOnTheCpu ? cpuTolerance : tolerance.maxAbs;
}

void logLoad(const std::string& what, double milliseconds, const Model& model)
{
    LOG(what, ": load ", milliseconds, " ms, cache hit ", model.wasCacheHit());
}

void logTimes(const std::string& what, Model& model, int context)
{
    auto& member = memberAt(context);
    auto first = encode(model, member).milliseconds;
    auto typical = medianPrediction(model, member, 9);

    LOG(what,
        " at ",
        context,
        ": first prediction ",
        first,
        " ms, median of 9 ",
        typical,
        " ms");
}
} // namespace

auto tEncoderMatchesReference =
    test("MLEncoder/whisperTinyMatchesTheReferenceOnEveryDevice") = []
{
    if (!isSupportedEncoder())
        return;

    auto& package = enumeratedPackage();
    check(!package.isEmpty());

    for (const auto& tolerance: encoderTolerances)
    {
        auto units = nameOf(tolerance.units);
        auto model = Model {};
        auto loadTime = timedLoad(model, package, tolerance.units, encoderCache());

        if (!model.isLoaded())
            return;

        logLoad("encoder [" + units + "]", loadTime, model);
        auto bound = boundFor(tolerance, model);

        for (const auto& member: checkedMembers())
        {
            auto encoded = encode(model, member);
            auto errors = compare(encoded.rows, member.expected);

            LOG("encoder at ",
                member.context,
                " [",
                units,
                "] maxAbs ",
                errors.maxAbs,
                " maxRel ",
                errors.maxRel);

            check(errors.maxAbs <= bound,
                  "encoder at " + std::to_string(member.context) + " under " + units
                      + ": max abs error " + std::to_string(errors.maxAbs)
                      + " exceeds " + std::to_string(bound));
        }
    }
};

auto tEncoderTimes = test("MLEncoder/whisperTinyLoadAndPredictionTimes") = []
{
    if (!isSupportedEncoder())
        return;

    auto& package = enumeratedPackage();

    for (auto units: {ComputeUnits::cpuAndNeuralEngine, ComputeUnits::all})
    {
        auto what = "encoder, 18 members [" + nameOf(units) + "]";
        auto cache = freshCacheDirectory("whisper-encoder-times-" + nameOf(units));

        auto cold = Model {};
        logLoad(what + " cold", timedLoad(cold, package, units, cache), cold);

        auto warm = Model {};
        logLoad(what + " warm", timedLoad(warm, package, units, cache), warm);
        check(warm.wasCacheHit());
    }

    for (auto units: {ComputeUnits::cpuAndNeuralEngine,
                      ComputeUnits::all,
                      ComputeUnits::cpuAndGPU,
                      ComputeUnits::cpu})
    {
        auto model = Model {};
        timedLoad(model, package, units, encoderCache());

        if (!model.isLoaded())
            return;

        logTimes("encoder [" + nameOf(units) + "]", model, 1500);
        logTimes("encoder [" + nameOf(units) + "]", model, 448);
    }
};

auto tEncoderOnTheEngine = test("MLEncoder/whisperTinyIsOnTheNeuralEngine") = []
{
    if (!canCheckPlacement())
        return;

    auto model = Model {};
    timedLoad(model,
              enumeratedPackage(),
              ComputeUnits::cpuAndNeuralEngine,
              encoderCache());

    auto start = Clock::now();
    auto plan = model.computePlan();
    LOG("enumerated encoder plan read in ", millisecondsSince(start), " ms");
    checkOnTheEngine("enumerated encoder [cpuAndNeuralEngine]", plan);

    auto everywhere = Model {};
    timedLoad(everywhere, enumeratedPackage(), ComputeUnits::all, encoderCache());
    LOG("enumerated encoder [all] placed: ",
        placementSummary(everywhere.computePlan()));
};

// MLComputePlan reads the compiled model and takes no shape, so what it says
// of an enumerated model cannot be per member. A program fixed at one context
// is placed on its own, which is where each size goes.
auto tEncoderMembersPlaced = test("MLEncoder/eachContextFixedIsPlacedOnItsOwn") = []
{
    if (!canCheckPlacement())
        return;

    for (auto context: {448, 1500})
    {
        auto package = WhisperEncoder::fixedEncoderGraph(context).build();
        auto cache =
            freshCacheDirectory("whisper-encoder-" + std::to_string(context));

        for (auto units: {ComputeUnits::cpuAndNeuralEngine, ComputeUnits::all})
        {
            auto what = "encoder fixed at " + std::to_string(context) + " ["
                        + nameOf(units) + "]";
            auto model = Model {};
            logLoad(what, timedLoad(model, package, units, cache), model);

            if (!model.isLoaded())
                return;

            auto& member = memberAt(context);
            auto errors = compare(encode(model, member).rows, member.expected);
            LOG(what, ": maxAbs ", errors.maxAbs);
            logTimes(what, model, context);

            if (units == ComputeUnits::cpuAndNeuralEngine)
                checkOnTheEngine(what, model.computePlan());
            else
                LOG(what, " placed: ", placementSummary(model.computePlan()));
        }
    }
};
