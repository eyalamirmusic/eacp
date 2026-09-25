#include "ModelTestCommon.h"

#include <chrono>
#include <memory>

using namespace nano;
using namespace ModelTests;

// The async forms: work on the model's own serial queue, results on the main
// thread, nothing delivered once the model is gone.
namespace
{
constexpr auto timeout = eacp::Time::MS {60000};
constexpr auto pollInterval = eacp::Time::MS {50};
constexpr auto settleTime = eacp::Time::MS {500};

constexpr auto rows = 16;
constexpr auto columns = 32;

Inputs inputsFor(const Vector<float>& x)
{
    auto inputs = Inputs {};
    inputs["x"] = arrayOf(x, {rows, columns}, DType::float16);
    return inputs;
}

bool hasCompiledModel(const FilePath& cache)
{
    auto isCompiled = [](const std::string& name)
    { return FilePath {name}.extension() == ".mlmodelc"; };

    return entriesOf(cache).findIf(isCompiled) != nullptr;
}

bool waitForCompiledModel(const FilePath& cache)
{
    auto deadline = eacp::Time::Deadline {timeout};

    while (!hasCompiledModel(cache))
    {
        if (deadline.expired())
            return false;

        eacp::Threads::runEventLoopFor(pollInterval);
    }

    return true;
}
} // namespace

auto tAsyncFormsResolveOnTheMainThread =
    test("MLAsync/loadAndPredictResolveOnTheMainThread") = []
{
    if (!isSupported())
        return;

    auto model = Model {};
    auto load = model.loadAsync(
        TestPrograms::elementwiseChain(rows, columns),
        optionsFor(ComputeUnits::all, freshCacheDirectory("async-load")));

    auto loadResolvedOnMain = false;
    auto noteLoad = [&loadResolvedOnMain](const Result&)
    { loadResolvedOnMain = eacp::Threads::isMainThread(); };
    load.then(noteLoad);

    auto loaded = load.waitFor(timeout);
    check(loaded.ok, loaded.error);
    check(loadResolvedOnMain);
    check(model.isLoaded());

    auto x = TestPrograms::seededValues(rows * columns, 11u, 1.0f);
    auto bound = Outputs {};
    bound["y"] = MultiArray::create({rows, columns}, DType::float16);

    auto predictResolvedOnMain = false;
    auto notePrediction = [&predictResolvedOnMain](const Prediction&)
    { predictResolvedOnMain = eacp::Threads::isMainThread(); };

    auto predict = model.predictAsync(inputsFor(x), bound);
    predict.then(notePrediction);

    auto prediction = predict.waitFor(timeout);
    check(prediction.ok, prediction.error);
    check(predictResolvedOnMain);

    auto expected = TestPrograms::elementwiseReference(x);
    check(compare(bound["y"].toFloats(), expected).maxAbs <= 2e-3,
          "the bound array was filled in place");
    check(prediction.outputs.getValue("y") != nullptr);
};

auto tQueuedPredictionFollowsItsLoad =
    test("MLAsync/aPredictionQueuedBehindALoadSeesTheModel") = []
{
    if (!isSupported())
        return;

    auto model = Model {};
    auto load = model.loadAsync(
        TestPrograms::elementwiseChain(rows, columns),
        optionsFor(ComputeUnits::cpu, freshCacheDirectory("async-order")));

    auto x = TestPrograms::seededValues(rows * columns, 13u, 1.0f);
    auto predict = model.predictAsync(inputsFor(x));

    auto prediction = predict.waitFor(timeout);
    check(load.isResolved());
    check(prediction.ok, prediction.error);

    auto y = prediction.outputs.getValue("y");
    check(y != nullptr);

    if (y != nullptr)
        check(compare(y->toFloats(), TestPrograms::elementwiseReference(x)).maxAbs
              <= 2e-3);
};

auto tPredictionTimesItselfOnTheQueue =
    test("MLAsync/aPredictionReportsItsQueueWaitAndItsRunTime") = []
{
    if (!isSupported())
        return;

    using Clock = std::chrono::steady_clock;

    auto model = Model {};
    auto loaded =
        model.load(TestPrograms::elementwiseChain(rows, columns),
                   optionsFor(ComputeUnits::cpu, freshCacheDirectory("async-time")));
    check(loaded.ok, loaded.error);

    auto x = TestPrograms::seededValues(rows * columns, 17u, 1.0f);
    auto called = Clock::now();
    auto resolved = called;
    auto noteResolve = [&resolved](const Prediction&) { resolved = Clock::now(); };

    auto first = model.predictAsync(inputsFor(x));
    first.then(noteResolve);
    auto second = model.predictAsync(inputsFor(x));

    auto firstPrediction = first.waitFor(timeout);
    auto secondPrediction = second.waitFor(timeout);
    check(firstPrediction.ok, firstPrediction.error);
    check(secondPrediction.ok, secondPrediction.error);

    auto wallSeconds = std::chrono::duration<double>(resolved - called).count();
    check(firstPrediction.predictSeconds > 0.0);
    check(firstPrediction.predictSeconds <= wallSeconds);
    check(firstPrediction.queueWaitSeconds >= 0.0);
    check(firstPrediction.queueWaitSeconds + firstPrediction.predictSeconds
          <= wallSeconds);
    check(secondPrediction.predictSeconds > 0.0);
    check(secondPrediction.queueWaitSeconds >= firstPrediction.predictSeconds,
          "the second waited behind the first");
};

auto tDestroyedModelAbandonsItsAsyncs =
    test("MLAsync/destroyingTheModelAbandonsWhatItHandedOut") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("async-abandon");
    auto model = std::make_unique<Model>();
    auto load = model->loadAsync(TestPrograms::elementwiseChain(rows, columns),
                                 optionsFor(ComputeUnits::cpu, cache));

    auto called = std::make_shared<bool>(false);
    auto noteCall = [called](const Result&) { *called = true; };
    load.then(noteCall);

    model.reset();

    check(waitForCompiledModel(cache), "the abandoned load still ran");
    eacp::Threads::runEventLoopFor(settleTime);

    check(!*called);
    check(!load.isReady());
};
