#include "DecoderStepReference.h"
#include "ModelTestCommon.h"

#include <chrono>

using namespace nano;
using namespace ModelTests;

// One tiny.en decode step, the program phase 4 would run once per token, on
// every compute-unit setting: checked against the fp32 reference at a short
// and a full cache, placed, and timed as a blocking prediction and as one
// predictAsync round trip, which is what phase 4's criterion is judged on.
// The same step with its caches baked in is timed beside it, as the bound on
// a stateful step.
namespace
{
using Clock = std::chrono::steady_clock;
using WhisperDecoderStep::StepOutputs;

constexpr auto timedRuns = 25;
constexpr auto warmUpRuns = 5;
constexpr auto asyncTimeout = eacp::Time::MS {60000};
constexpr auto checkedPrefixes = Array<int, 2> {{1, 447}};

double microsecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

struct Times
{
    double median = 0.0;
    double min = 0.0;
};

Times timesOf(Vector<double> values)
{
    std::sort(values.begin(), values.end());
    return values.empty() ? Times {} : Times {values[values.size() / 2], values[0]};
}

int tokenFor(int prefix)
{
    return (prefix * 7919 + 50257) % WhisperDecoderStep::vocabulary;
}

enum class Caches
{
    asInputs,
    resident
};

const Package& packageFor(Caches caches)
{
    static const auto withInputs = WhisperDecoderStep::sharedStepGraph().build();
    static const auto resident =
        WhisperDecoderStep::residentCacheStepGraph().build();
    return caches == Caches::asInputs ? withInputs : resident;
}

std::string programName(Caches caches)
{
    return caches == Caches::asInputs ? "decoder step" : "resident-cache step";
}

const FilePath& stepCache()
{
    static const auto cache = freshCacheDirectory("whisper-decoder-step");
    return cache;
}

bool isSupportedStep()
{
    return supportsSpecification(9);
}

struct Bound
{
    Inputs inputs;
    Outputs outputs;
};

Inputs cacheInputs()
{
    auto& caches = WhisperDecoderStep::sharedCaches();
    auto selfShape = WhisperDecoderStep::selfCacheShape();
    auto crossShape = WhisperDecoderStep::crossCacheShape();

    auto inputs = Inputs {};
    inputs["self_keys"] = arrayOf(caches.selfKeys, selfShape, DType::float16);
    inputs["self_values"] = arrayOf(caches.selfValues, selfShape, DType::float16);
    inputs["cross_keys"] = arrayOf(caches.crossKeys, crossShape, DType::float16);
    inputs["cross_values"] = arrayOf(caches.crossValues, crossShape, DType::float16);
    return inputs;
}

// The caches are shared by every step, as a sequence's would be; only the
// token, the position and the mask change with the prefix.
Bound boundAt(int prefix, Caches caches = Caches::asInputs)
{
    auto token = Vector<float> {(float) tokenFor(prefix)};
    auto position = Vector<float> {(float) prefix};

    auto bound = Bound {caches == Caches::asInputs ? cacheInputs() : Inputs {}, {}};
    bound.inputs["token"] = arrayOf(token, {1}, DType::int32);
    bound.inputs["position"] = arrayOf(position, {1}, DType::int32);
    bound.inputs["mask"] = arrayOf(WhisperDecoderStep::maskFor(prefix),
                                   {1, WhisperDecoderStep::maxPositions},
                                   DType::float16);

    auto rowsShape =
        Shape {WhisperDecoderStep::layerCount, WhisperDecoderStep::width};
    bound.outputs["logits"] =
        MultiArray::create({1, WhisperDecoderStep::vocabulary}, DType::float16);
    bound.outputs["new_keys"] = MultiArray::create(rowsShape, DType::float16);
    bound.outputs["new_values"] = MultiArray::create(rowsShape, DType::float16);
    return bound;
}

const StepOutputs& expectedAt(int prefix)
{
    static auto expected = EA::MapVector<int, StepOutputs> {};

    if (auto* found = expected.getValue(prefix))
        return *found;

    auto start = Clock::now();
    expected[prefix] = WhisperDecoderStep::referenceStep(
        WhisperDecoderStep::sharedWeights(),
        WhisperDecoderStep::stepAt(prefix, tokenFor(prefix)));
    LOG("decoder step reference at prefix ",
        prefix,
        ": ",
        microsecondsSince(start) / 1000.0,
        " ms");
    return expected[prefix];
}

double timedPredict(Model& model, Bound& bound)
{
    auto start = Clock::now();
    auto result = model.predict(bound.inputs, bound.outputs);
    auto elapsed = microsecondsSince(start);
    check(result.ok, result.error);
    return elapsed;
}

struct RoundTrips
{
    Times toResolve;
    Times toWaitReturn;
};

// The call to the moment its continuation runs on the main thread, which is
// the hop a decoder driven from the loop would pay, and to the moment waitFor
// returns, which adds the nested pump's own exit.
RoundTrips asyncRoundTrips(Model& model, Bound& bound)
{
    auto toResolve = Vector<double> {};
    auto toWaitReturn = Vector<double> {};

    for (auto run = 0; run < warmUpRuns + timedRuns; ++run)
    {
        auto start = Clock::now();
        auto resolved = start;
        auto noteResolve = [&resolved](const Prediction&)
        { resolved = Clock::now(); };

        auto pending = model.predictAsync(bound.inputs, bound.outputs);
        pending.then(noteResolve);
        auto prediction = pending.waitFor(asyncTimeout);
        auto returned = microsecondsSince(start);
        check(prediction.ok, prediction.error);

        if (run < warmUpRuns)
            continue;

        toResolve.add(
            std::chrono::duration<double, std::micro>(resolved - start).count());
        toWaitReturn.add(returned);
    }

    return {timesOf(toResolve), timesOf(toWaitReturn)};
}

bool load(Model& model, ComputeUnits units, Caches caches = Caches::asInputs)
{
    auto start = Clock::now();
    auto loaded = model.load(packageFor(caches), optionsFor(units, stepCache()));
    check(loaded.ok, loaded.error);
    LOG(programName(caches),
        " [",
        nameOf(units),
        "]: load ",
        microsecondsSince(start) / 1000.0,
        " ms, cache hit ",
        model.wasCacheHit());
    return loaded.ok;
}

struct Tolerance
{
    ComputeUnits units;
    double maxAbs;
};

// About three times what was measured over the logits and the appended rows
// at prefixes 1 and 447: CPU 7.9e-2, GPU 8.5e-3, Neural Engine 2.8e-2. The
// engine settings keep their own bound only under EACP_REQUIRE_ANE=1, as the
// encoder's do.
constexpr auto cpuTolerance = 0.25;

const auto stepTolerances = Array<Tolerance, 4> {{
    {ComputeUnits::cpu, cpuTolerance},
    {ComputeUnits::cpuAndGPU, 0.03},
    {ComputeUnits::cpuAndNeuralEngine, 0.09},
    {ComputeUnits::all, 0.09},
}};

bool isEngineSetting(ComputeUnits units)
{
    return units == ComputeUnits::cpuAndNeuralEngine || units == ComputeUnits::all;
}

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

double worstError(const Bound& bound, const StepOutputs& expected)
{
    auto logits =
        compare(bound.outputs.getValue("logits")->toFloats(), expected.logits);
    auto keys =
        compare(bound.outputs.getValue("new_keys")->toFloats(), expected.newKeys);
    auto values = compare(bound.outputs.getValue("new_values")->toFloats(),
                          expected.newValues);

    LOG("  logits maxAbs ",
        logits.maxAbs,
        ", new keys ",
        keys.maxAbs,
        ", new values ",
        values.maxAbs);
    return std::max({logits.maxAbs, keys.maxAbs, values.maxAbs});
}

bool isHeavyOp(const std::string& type)
{
    return type == "linear" || type == "matmul" || type == "softmax"
           || type == "layer_norm" || type == "scaled_dot_product_attention";
}

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

Vector<std::string> heavyOpsOffTheEngine(const ComputePlan& plan)
{
    auto misplaced = Vector<std::string> {};

    for (const auto& op: plan.ops)
        if (isHeavyOp(op.type) && op.device != ComputePlan::Device::neuralEngine)
            misplaced.add(op.type + " " + op.name + " on " + toString(op.device));

    return misplaced;
}

bool canCheckPlacement()
{
    if (isSupportedStep() && hasComputePlan() && hasNeuralEngine())
        return true;

    check(!isAneRequired(), "no CoreML8, compute plan or engine to place on");
    return false;
}

const auto everySetting = Array<ComputeUnits, 4> {{ComputeUnits::cpu,
                                                   ComputeUnits::cpuAndGPU,
                                                   ComputeUnits::cpuAndNeuralEngine,
                                                   ComputeUnits::all}};

void checkAgainstReference(Caches caches)
{
    check(!packageFor(caches).isEmpty());

    for (const auto& tolerance: stepTolerances)
    {
        auto units = nameOf(tolerance.units);
        auto model = Model {};

        if (!load(model, tolerance.units, caches))
            return;

        auto bound = boundFor(tolerance, model);

        for (auto prefix: checkedPrefixes)
        {
            auto step = boundAt(prefix, caches);
            timedPredict(model, step);
            LOG(programName(caches), " at prefix ", prefix, " [", units, "]");
            auto error = worstError(step, expectedAt(prefix));

            check(error <= bound,
                  programName(caches) + " at prefix " + std::to_string(prefix)
                      + " under " + units + ": max abs error "
                      + std::to_string(error) + " exceeds " + std::to_string(bound));
        }
    }
}

void logBlockingTimes(Model& model, ComputeUnits units, Caches caches)
{
    for (auto prefix: checkedPrefixes)
    {
        auto step = boundAt(prefix, caches);
        auto first = timedPredict(model, step);

        for (auto run = 0; run < warmUpRuns; ++run)
            timedPredict(model, step);

        auto times = Vector<double> {};

        for (auto run = 0; run < timedRuns; ++run)
            times.add(timedPredict(model, step));

        auto blocking = timesOf(times);
        LOG(programName(caches),
            " [",
            nameOf(units),
            "] prefix ",
            prefix,
            ": first ",
            first,
            " us, predict median ",
            blocking.median,
            " us, min ",
            blocking.min,
            " us over ",
            timedRuns);
    }
}
} // namespace

auto tStepMatchesReference =
    test("MLDecoderStep/tinyEnStepMatchesTheReferenceOnEveryDevice") = []
{
    if (isSupportedStep())
        checkAgainstReference(Caches::asInputs);
};

// The caches baked into the program: the same step for the one set of caches
// the suite uses, with only the token, the position and the mask crossing in.
auto tResidentStepMatchesReference =
    test("MLDecoderStep/residentCacheStepMatchesTheReference") = []
{
    if (isSupportedStep())
        checkAgainstReference(Caches::resident);
};

auto tStepTimes = test("MLDecoderStep/tinyEnStepPredictionTimes") = []
{
    if (!isSupportedStep())
        return;

    for (auto units: everySetting)
    {
        auto model = Model {};

        if (!load(model, units))
            return;

        logBlockingTimes(model, units, Caches::asInputs);

        auto step = boundAt(1);
        auto trips = asyncRoundTrips(model, step);
        LOG("decoder step [",
            nameOf(units),
            "] predictAsync to its resolve on the main thread: median ",
            trips.toResolve.median,
            " us, min ",
            trips.toResolve.min,
            " us; to waitFor's return: median ",
            trips.toWaitReturn.median,
            " us, over ",
            timedRuns);
    }
};

auto tResidentStepTimes = test("MLDecoderStep/residentCacheStepPredictionTimes") = []
{
    if (!isSupportedStep())
        return;

    for (auto units: everySetting)
    {
        auto model = Model {};

        if (load(model, units, Caches::resident))
            logBlockingTimes(model, units, Caches::resident);
    }
};

auto tStepPlacement = test("MLDecoderStep/tinyEnStepPlacement") = []
{
    if (!canCheckPlacement())
        return;

    for (auto units: everySetting)
    {
        auto model = Model {};

        if (!load(model, units))
            return;

        auto start = Clock::now();
        auto plan = model.computePlan();
        auto what = "decoder step [" + nameOf(units) + "]";
        LOG(what,
            " plan read in ",
            microsecondsSince(start) / 1000.0,
            " ms, placed: ",
            placementSummary(plan));

        if (units != ComputeUnits::cpuAndNeuralEngine)
            continue;

        check(!plan.isEmpty() || !isAneRequired(), what + ": no compute plan");
        auto misplaced = heavyOpsOffTheEngine(plan);

        if (misplaced.empty())
            continue;

        if (!isAneRequired())
        {
            LOG("skipped: ", what, " has ", misplaced.size(), " ops off the engine");
            continue;
        }

        check(false,
              what + ": " + misplaced[0] + " and "
                  + std::to_string(misplaced.size() - 1)
                  + " more off the Neural Engine");
    }
};
