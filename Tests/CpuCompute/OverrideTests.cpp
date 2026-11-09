#include "CountingHeap.h"

#include <NanoTest/NanoTest.h>

#include <eacp/GPU/Codegen/PackedScalars.h>
#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <source_location>
#include <span>
#include <string>
#include <string_view>

// WGSL's pipeline-overridable constants on the CPU executor: the values a GPU
// pipeline would be specialised with, given to the plan by name and resolved
// once into constants of it. Defaults where nothing is given, two value sets
// giving two results from one kernel, an index scaled by an override, every
// refusal naming the override, and a dispatch that allocates nothing.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace OverrideTests
{
namespace
{
constexpr auto threadCount = 37;
constexpr auto sentinel = -7.f;

const auto batchings = std::array {PlanOptions {}, PlanOptions {1}};

struct OverrideKernel final : ComputeKernel
{
    OverrideKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto gain = overrideConstant<Float>("gain", 2.f);
        auto bias = overrideConstant<Int>("bias", -1);
        auto stride = overrideConstant<UInt>("stride");
        auto flip = overrideConstant<Bool>("flip", false);
        auto step = overrideConstant<Half>("step", 0.25f);

        auto value = input[id] * gain + toFloat(bias) + toFloat(step);
        write(output, id, select(flip, -value, value));
        write(strided, id, input[id * stride]);
        write(indices, id, id * stride);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<OutputBuffer> strided;
    Uniform<UIntOutputBuffer> indices;

    EACP_SHADER(input, output, strided, indices)
};

struct PlainKernel final : ComputeKernel
{
    PlainKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        write(output, id, input[id] + 1.f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct Expected
{
    float gain = 2.f;
    int bias = -1;
    unsigned stride = 1;
    bool flip = false;
    float step = 0.25f;
};

PlanOptions withValues(const OverrideValues& values,
                       PlanOptions options = PlanOptions {})
{
    options.overrides = values;
    return options;
}

float inputAt(int index)
{
    return 0.5f * (float) index - 3.f;
}

struct Run
{
    explicit Run(OverrideKernel& kernel)
        : bindings()
    {
        for (auto index = 0; index < threadCount; ++index)
            input[(std::size_t) index] = inputAt(index);

        output.fill(sentinel);
        strided.fill(sentinel);
        indices.fill(0u);

        bindings.set(kernel.input, std::span<const float> {input});
        bindings.set(kernel.output, std::span<float> {output});
        bindings.set(kernel.strided, std::span<float> {strided});
        bindings.set(kernel.indices, std::span<std::uint32_t> {indices});
    }

    std::array<float, threadCount> input {};
    std::array<float, threadCount> output {};
    std::array<float, threadCount> strided {};
    std::array<std::uint32_t, threadCount> indices {};
    Bindings bindings;
};

void expectRun(const Run& run,
               const Expected& expected,
               const std::source_location& where = std::source_location::current())
{
    for (auto index = 0; index < threadCount; ++index)
    {
        const auto at = (std::size_t) index;
        const auto value =
            inputAt(index) * expected.gain + (float) expected.bias + expected.step;
        const auto scaled = (std::uint32_t) index * expected.stride;
        const auto read =
            scaled < (std::uint32_t) threadCount ? inputAt((int) scaled) : 0.f;

        check(run.output[at] == (expected.flip ? -value : value),
              "output " + std::to_string(index),
              where);
        check(run.indices[at] == scaled, "index " + std::to_string(index), where);
        check(run.strided[at] == read, "strided " + std::to_string(index), where);
    }
}

std::string refusalOf(OverrideKernel& kernel, const OverrideValues& values)
{
    auto executor = Executor {kernel, withValues(values)};
    auto run = Run {kernel};

    check(!executor.isValid());
    check(!executor.dispatch(run.bindings, threadCount), "nothing runs");
    check(run.output[0] == sentinel);
    return executor.reason();
}

bool contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

auto tDefaults = test("Override/defaultsApplyWhereNoValueIsGiven") = []
{
    auto kernel = OverrideKernel {};

    for (const auto& batching: batchings)
    {
        auto executor = Executor {
            kernel, withValues(OverrideValues {}.set("stride", 3u), batching)};
        auto run = Run {kernel};

        check(executor.isValid(), executor.reason());
        check(executor.dispatch(run.bindings, threadCount));
        expectRun(run, Expected {2.f, -1, 3u, false, 0.25f});
    }
};

auto tTwoSets = test("Override/twoValueSetsGiveTwoResults") = []
{
    auto kernel = OverrideKernel {};

    const auto first =
        OverrideValues {}.set("stride", 2u).set("gain", 0.5f).set("flip", true);
    const auto second = OverrideValues {}
                            .set("stride", 0u)
                            .set("gain", -4.f)
                            .set("bias", 5)
                            .set("step", 0.1f);

    for (const auto& batching: batchings)
    {
        auto firstExecutor = Executor {kernel, withValues(first, batching)};
        auto secondExecutor = Executor {kernel, withValues(second, batching)};
        auto firstRun = Run {kernel};
        auto secondRun = Run {kernel};

        check(firstExecutor.dispatch(firstRun.bindings, threadCount),
              firstExecutor.reason());
        check(secondExecutor.dispatch(secondRun.bindings, threadCount),
              secondExecutor.reason());

        expectRun(firstRun, Expected {0.5f, -1, 2u, true, 0.25f});
        expectRun(secondRun,
                  Expected {-4.f, 5, 0u, false, halfToFloat(halfFromFloat(0.1f))});
    }
};

auto tLargest = test("Override/integerValuesKeepEveryBit") = []
{
    auto kernel = OverrideKernel {};
    auto executor = Executor {
        kernel,
        withValues(
            OverrideValues {}.set("stride", 0xffffffffu).set("bias", -1000000))};
    auto run = Run {kernel};

    check(executor.dispatch(run.bindings, threadCount), executor.reason());
    expectRun(run, Expected {2.f, -1000000, 0xffffffffu, false, 0.25f});
};

auto tRefused = test("Override/valuesTheKernelRefusesLeaveNoPlan") = []
{
    auto kernel = OverrideKernel {};
    const auto withStride = [] { return OverrideValues {}.set("stride", 1u); };

    check(contains(refusalOf(kernel, OverrideValues {}), "\"stride\" (UInt)"));
    check(contains(refusalOf(kernel, withStride().set("gian", 1.f)),
                   "no override named \"gian\""));
    check(contains(refusalOf(kernel, withStride().set("gain", 1)),
                   "\"gain\" (Float)"));
    check(contains(refusalOf(kernel, withStride().set("flip", 1.f)),
                   "\"flip\" (Bool)"));
    check(contains(refusalOf(kernel, withStride().set("bias", true)),
                   "\"bias\" (Int)"));
    check(contains(refusalOf(kernel, OverrideValues {}.set("stride", -2)),
                   "negative"));
    check(contains(refusalOf(kernel, withStride().set("step", 1e6f)), "f16 range"));
    check(contains(
        refusalOf(kernel,
                  withStride().set("gain", std::numeric_limits<float>::infinity())),
        "not finite"));

    auto plain = PlainKernel {};
    auto executor =
        Executor {plain, withValues(OverrideValues {}.set("anything", 1.f))};
    check(!executor.isValid() && contains(executor.reason(), "\"anything\""),
          executor.reason());
};

auto tNoAllocation = test("Override/aDispatchAllocatesNothing") = []
{
    auto kernel = OverrideKernel {};
    auto executor = Executor {
        kernel, withValues(OverrideValues {}.set("stride", 2u).set("gain", 3.f))};
    auto run = Run {kernel};

    auto warmedUp = executor.dispatch(run.bindings, threadCount);

    ExecutorTests::allocationsCounted = 0;
    ExecutorTests::countingHeap = true;
    auto ran = executor.dispatch(run.bindings, threadCount);
    ExecutorTests::countingHeap = false;

    check(warmedUp);
    check(ran);
    check(ExecutorTests::allocationsCounted.load() == 0);
    expectRun(run, Expected {3.f, -1, 2u, false, 0.25f});
};
} // namespace
} // namespace OverrideTests
