#include "ModelTestCommon.h"

using namespace nano;
using namespace ModelTests;

// What the running OS and machine have, and where Core ML put the ops. A
// placement that is not the engine is a skip unless EACP_REQUIRE_ANE=1 says
// this machine must have one, as EACP_REQUIRE_GPU=1 does for the device
// suites: otherwise a machine with no engine reports a green suite that
// asserted nothing.
namespace
{
void logPlan(const std::string& what, const ComputePlan& plan)
{
    for (const auto& op: plan.ops)
        LOG(what,
            ": ",
            op.type,
            " ",
            op.name,
            " on ",
            toString(op.device),
            " cost ",
            op.cost);
}

void checkOnEngine(ComputeUnits units)
{
    auto net = TestPrograms::linearSoftmaxWeights(1500, 384);
    auto model = Model {};
    auto loaded = model.load(TestPrograms::linearSoftmax(net),
                             optionsFor(units, freshCacheDirectory("placement")));
    check(loaded.ok, loaded.error);

    if (!loaded)
        return;

    auto plan = model.computePlan();
    logPlan("plan [" + nameOf(units) + "]", plan);

    auto linear = plan.find("linear");
    auto softmax = plan.find("softmax");
    check(linear != nullptr && softmax != nullptr, "the plan lists both ops");

    if (linear == nullptr || softmax == nullptr)
        return;

    auto onEngine = plan.allOn(ComputePlan::Device::neuralEngine);

    if (!onEngine && !isAneRequired())
    {
        LOG("skipped: linear+softmax not on the Neural Engine under ",
            nameOf(units));
        return;
    }

    check(linear->device == ComputePlan::Device::neuralEngine,
          "linear on " + toString(linear->device) + " under " + nameOf(units));
    check(softmax->device == ComputePlan::Device::neuralEngine,
          "softmax on " + toString(softmax->device) + " under " + nameOf(units));
}
} // namespace

auto tAvailabilityIsHonest = test("MLPlacement/availabilityMatchesTheRunningOS") = []
{
    auto version = osVersion();
    LOG("OS ",
        version.major,
        ".",
        version.minor,
        ": isSupported ",
        isSupported(),
        " hasComputePlan ",
        hasComputePlan(),
        " hasNeuralEngine ",
        hasNeuralEngine());

    check(isSupported() == version.atLeast(isIOS() ? 16 : 13, 0));
    check(hasComputePlan() == version.atLeast(isIOS() ? 17 : 14, 4));
};

auto tSpecificationSupportIsHonest =
    test("MLPlacement/specificationSupportMatchesTheRunningOS") = []
{
    auto version = osVersion();

    check(supportsSpecification(5));
    check(supportsSpecification(7) == isSupported());
    check(supportsSpecification(8) == version.atLeast(isIOS() ? 17 : 14, 0));
    check(supportsSpecification(9) == version.atLeast(isIOS() ? 18 : 15, 0));
    check(!supportsSpecification(10));

    auto graph = Graph {};
    auto q = graph.input("q", {1, 4, 8}, DType::float16);
    graph.output(graph.scaledDotProductAttention(q, q, q, false), "y");
    auto needed = graph.specification().specificationVersion;
    check(needed == 9);

    if (!supportsSpecification(needed))
        return;

    auto model = Model {};
    auto loaded = model.load(
        graph.build(), optionsFor(ComputeUnits::cpu, freshCacheDirectory("spec")));
    check(loaded.ok, loaded.error);
};

auto tEngineIsPresentWhenRequired =
    test("MLPlacement/anEngineIsPresentWhenRequired") = []
{
    if (!isAneRequired())
        return;

    check(hasNeuralEngine(),
          "EACP_REQUIRE_ANE=1 but Core ML lists no Neural Engine - every placement "
          "test would otherwise have skipped");
};

auto tLinearAndSoftmaxOnTheEngine =
    test("MLPlacement/linearAndSoftmaxAreOnTheNeuralEngine") = []
{
    if (!hasComputePlan() || !hasNeuralEngine())
    {
        check(!isAneRequired(), "no compute plan or no engine to place on");
        return;
    }

    checkOnEngine(ComputeUnits::cpuAndNeuralEngine);
    checkOnEngine(ComputeUnits::all);
};

auto tCpuOnlyStaysOnTheCpu = test("MLPlacement/aCpuOnlyModelIsPlacedOnTheCpu") = []
{
    if (!hasComputePlan())
        return;

    auto model = Model {};
    auto loaded =
        model.load(TestPrograms::elementwiseChain(8, 16),
                   optionsFor(ComputeUnits::cpu, freshCacheDirectory("cpu-plan")));
    check(loaded.ok, loaded.error);

    auto plan = model.computePlan();
    logPlan("plan [cpu]", plan);
    check(plan.find("tanh") != nullptr);
    check(plan.find("const") == nullptr, "the constants are left out");
    check(plan.allOn(ComputePlan::Device::cpu));
};
