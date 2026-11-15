#include "CountingHeap.h"

#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>
#include <eacp/GPU/CpuCompute/Helpers.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>

// WGSL's user functions on the CPU executor, which runs no call: each one is
// the function's body copied to where it is called, its parameters declared
// from the arguments and a return in it ending the body alone. Early returns
// from an if, a loop and a switch with lanes taking each; a function calling
// another; calls in a loop's condition, an if's and a switch's selector,
// evaluated afresh each time; two calls in one expression and a call of a
// call; void functions that store and count and a value function reading what
// the kernel wrote; structs, f16 and matrices by value; and a dispatch that
// allocates nothing.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace FunctionTests
{
namespace
{
constexpr auto threadCount = 37;
constexpr auto sentinel = -7.f;
constexpr auto uintSentinel = 0xdeadbeefu;

const auto batchings = std::array {PlanOptions {}, PlanOptions {1}};

float inputAt(int index)
{
    return 0.125f * (float) index - 1.5f;
}

std::array<float, threadCount> inputs()
{
    auto values = std::array<float, threadCount> {};

    for (auto index = 0; index < threadCount; ++index)
        values[(std::size_t) index] = inputAt(index);

    return values;
}

template <typename T>
std::array<T, threadCount> filledWith(T value)
{
    auto values = std::array<T, threadCount> {};
    values.fill(value);
    return values;
}

// clampUnit returns early below zero; blend calls it twice.
struct BlendKernel final : ComputeKernel
{
    BlendKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto clampUnit = function<Float(Float)>("clampUnit",
                                                [&](Float x)
                                                {
                                                    auto belowZero = [&]
                                                    { returnEarly(constant(0.f)); };
                                                    ifThen(x < 0.f, belowZero);
                                                    return min(x, 1.f);
                                                });

        auto blend = function<Float(Float, Float)>(
            "blend",
            [&](Float a, Float b)
            { return clampUnit(a) * 0.25f + clampUnit(b) * 0.75f; });

        write(output, id, blend(input[id], input[id] * -2.f + 1.f));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    EACP_SHADER(input, output)
};

float clampUnit(float x)
{
    if (x < 0.f)
        return 0.f;

    return std::min(x, 1.f);
}

float expectedBlend(int index)
{
    auto x = inputAt(index);
    return clampUnit(x) * 0.25f + clampUnit(x * -2.f + 1.f) * 0.75f;
}

// Returns from inside a loop, from a switch's clause and at the end.
struct SearchKernel final : ComputeKernel
{
    SearchKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto firstSquareAbove = function<UInt(UInt, UInt)>(
            "firstSquareAbove",
            [&](UInt start, UInt limit)
            {
                auto at = var(start);
                auto found = [&] { returnEarly(at.get()); };
                auto special = [&] { returnEarly(unsignedInteger(500u)); };
                auto step = [&]
                {
                    ifThen(at.get() * at.get() > limit, found);
                    switchOn(at.get(), caseOf(9, special));
                    at += 1u;
                };

                loop(at.get() < 12u, step);
                return unsignedInteger(999u);
            });

        auto twice = function<UInt(UInt)>("twice", [&](UInt x) { return x * 2u; });

        auto count = var(0u);
        auto advance = [&] { count += 1u; };
        loop(twice(count.get()) < id % 13u, advance);

        auto mode = var(0u);
        auto odd = [&] { mode = 1u; };
        ifThen(twice(id) % 4u == 2u, odd);

        auto picked = var(0u);
        auto zero = [&] { picked = 10u; };
        auto other = [&] { picked = 20u; };
        switchOn(twice(id % 3u), caseOf(0, zero), defaultCase(other));

        write(found, id, firstSquareAbove(id % 5u, id * 3u));
        write(counts, id, count.get() + mode.get() * 100u + picked.get() * 1000u);
    }

    Uniform<UIntOutputBuffer> found;
    Uniform<UIntOutputBuffer> counts;
    EACP_SHADER(found, counts)
};

std::uint32_t firstSquareAbove(std::uint32_t start, std::uint32_t limit)
{
    for (auto at = start; at < 12u; ++at)
    {
        if (at * at > limit)
            return at;

        if (at == 9u)
            return 500u;
    }

    return 999u;
}

std::uint32_t expectedCounts(std::uint32_t id)
{
    auto count = 0u;

    while (count * 2u < id % 13u)
        ++count;

    auto mode = (id * 2u) % 4u == 2u ? 1u : 0u;
    auto picked = (id % 3u) * 2u == 0u ? 10u : 20u;
    return count + mode * 100u + picked * 1000u;
}

// Calls in a loop's body and in its continuing block, break-if's included.
struct ContinuingKernel final : ComputeKernel
{
    ContinuingKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto addOne = function<UInt(UInt)>("addOne", [&](UInt x) { return x + 1u; });

        auto counter = var(0u);
        auto total = var(0u);
        auto body = [&] { total += addOne(counter.get()); };
        auto update = [&]
        {
            counter += 1u;
            breakLoopIf(addOne(counter.get()) > id % 7u + 1u);
        };

        loop(counter.get() < 20u, body, update);
        write(output, id, total.get() * 100u + counter.get());
    }

    Uniform<UIntOutputBuffer> output;
    EACP_SHADER(output)
};

std::uint32_t expectedContinuing(std::uint32_t id)
{
    auto counter = 0u;
    auto total = 0u;

    while (counter < 20u)
    {
        total += counter + 1u;
        counter += 1u;

        if (counter + 1u > id % 7u + 1u)
            break;
    }

    return total * 100u + counter;
}

// A constant array's elements are evaluated once, ahead of every statement,
// which the CPU executor has no place to inline a call in front of.
struct ArrayCallKernel
{
    ArrayCallKernel()
    {
        auto output = builder.outputBuffer();
        auto id = builder.threadId();
        auto twice = builder.function<Float(Float)>(
            "twice", [&](Float x) { return x * 2.f; });
        auto table =
            builder.array(twice(builder.constant(1.f)), builder.constant(3.f));
        builder.write(output, id, table[toInt(id % 2u)]);
    }

    ShaderBuilder builder;
};

// Two calls in one expression, and a call of a call.
struct SquareKernel final : ComputeKernel
{
    SquareKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto square =
            function<Float(Float)>("square", [&](Float x) { return x * x; });
        auto x = input[id];

        write(output, id, square(x) - square(x + 1.f) + square(square(x * 0.5f)));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    EACP_SHADER(input, output)
};

float expectedSquares(int index)
{
    auto x = inputAt(index);
    auto half = (x * 0.5f) * (x * 0.5f);
    return x * x - (x + 1.f) * (x + 1.f) + half * half;
}

// A void function that stores, counts and returns early; a value function
// that reads what the kernel wrote, which makes its call a statement.
struct RecordKernel final : ComputeKernel
{
    RecordKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto record =
            function<void(UInt, Float)>("record",
                                        [&](UInt at, Float value)
                                        {
                                            write(first, at, value);
                                            atomicAdd(counter, 0u, 1u);

                                            auto leave = [&] { returnEarly(); };
                                            ifThen(value > 0.5f, leave);
                                            write(second, at, value * 2.f);
                                        });

        auto readBack = function<Float(UInt)>(
            "readBack", [&](UInt at) { return first[at] + 1.f; });

        record(id, input[id]);
        write(third, id, readBack(id));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> first;
    Uniform<OutputBuffer> second;
    Uniform<OutputBuffer> third;
    Uniform<AtomicBuffer> counter;
    EACP_SHADER(input, first, second, third, counter)
};

struct Body
{
    Float3 position;
    Float mass;

    EACP_SHADER_STRUCT(Body, position, mass)
};

// A struct in and out, f16 values and a matrix.
struct ValueKernel final : ComputeKernel
{
    ValueKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto weigh = function<Body(Body, Float)>(
            "weigh",
            [&](Body body, Float k)
            { return Body {body.position * k, body.mass + k}; });

        auto mixHalf = function<Half(Half, Half2)>(
            "mixHalf", [&](Half h, Half2 v) { return h * v.x() + v.y(); });

        auto turn = function<Float2(Float2x2, Float2)>(
            "turn", [&](Float2x2 m, Float2 v) { return m * v; });

        auto k = toFloat(id);
        write(bodies, id, weigh(input[id], k));

        auto h = halves[id];
        write(halfOutput, id, mixHalf(h, half2(h, toHalf(k))));

        auto rotation =
            float2x2(float2(constant(0.f), 1.f), float2(constant(-1.f), 0.f));
        auto turned = turn(rotation, float2(k, 1.f));
        write(turns, id, turned.x() * 10.f + turned.y());
    }

    Uniform<StructInputBuffer<Body>> input;
    Uniform<StructOutputBuffer<Body>> bodies;
    Uniform<HalfInputBuffer> halves;
    Uniform<HalfOutputBuffer> halfOutput;
    Uniform<OutputBuffer> turns;
    EACP_SHADER(input, bodies, halves, halfOutput, turns)
};

auto tBlend = test("Function/aCallRunsItsBodyAndReturnsEarly") = []
{
    auto kernel = BlendKernel {};

    for (const auto& options: batchings)
    {
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto input = inputs();
        auto output = filledWith(sentinel);
        auto bindings = Bindings {};
        bindings.set(kernel.input, std::span<const float> {input});
        bindings.set(kernel.output, std::span<float> {output});
        check(executor.dispatch(bindings, threadCount));

        for (auto index = 0; index < threadCount; ++index)
            check(output[(std::size_t) index] == expectedBlend(index),
                  "thread " + std::to_string(index));
    }
};

auto tSearch = test("Function/returnsLeaveLoopsAndSwitchesAndCallsRunAfresh") = []
{
    auto kernel = SearchKernel {};

    for (const auto& options: batchings)
    {
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto found = filledWith(uintSentinel);
        auto counts = filledWith(uintSentinel);
        auto bindings = Bindings {};
        bindings.set(kernel.found, std::span<std::uint32_t> {found});
        bindings.set(kernel.counts, std::span<std::uint32_t> {counts});
        check(executor.dispatch(bindings, threadCount));

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            check(found[id] == firstSquareAbove(id % 5u, id * 3u),
                  "found " + std::to_string(id));
            check(counts[id] == expectedCounts(id), "counts " + std::to_string(id));
        }
    }
};

auto tContinuing = test("Function/callsRunInAContinuingBlockAndItsBreakIf") = []
{
    auto kernel = ContinuingKernel {};

    for (const auto& options: batchings)
    {
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto output = filledWith(uintSentinel);
        auto bindings = Bindings {};
        bindings.set(kernel.output, std::span<std::uint32_t> {output});
        check(executor.dispatch(bindings, threadCount));

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
            check(output[id] == expectedContinuing(id),
                  "thread " + std::to_string(id));
    }
};

auto tArray = test("Function/aCallInAConstantArrayIsRefusedByName") = []
{
    auto kernel = ArrayCallKernel {};
    auto executor = Executor {kernel.builder.graph()};

    check(!executor.isValid());
    check(executor.reason().find("constant array") != std::string::npos,
          executor.reason());
};

auto tSquares = test("Function/twoCallsInOneExpressionKeepTwoValues") = []
{
    auto kernel = SquareKernel {};

    for (const auto& options: batchings)
    {
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto input = inputs();
        auto output = filledWith(sentinel);
        auto bindings = Bindings {};
        bindings.set(kernel.input, std::span<const float> {input});
        bindings.set(kernel.output, std::span<float> {output});
        check(executor.dispatch(bindings, threadCount));

        for (auto index = 0; index < threadCount; ++index)
            check(output[(std::size_t) index] == expectedSquares(index),
                  "thread " + std::to_string(index));
    }
};

auto tRecord = test("Function/voidFunctionsStoreAndCount") = []
{
    auto kernel = RecordKernel {};

    for (const auto& options: batchings)
    {
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto input = inputs();
        auto first = filledWith(sentinel);
        auto second = filledWith(sentinel);
        auto third = filledWith(sentinel);
        auto counter = std::array<std::uint32_t, 1> {0u};
        auto bindings = Bindings {};
        bindings.set(kernel.input, std::span<const float> {input});
        bindings.set(kernel.first, std::span<float> {first});
        bindings.set(kernel.second, std::span<float> {second});
        bindings.set(kernel.third, std::span<float> {third});
        bindings.set(kernel.counter, std::span<std::uint32_t> {counter});
        check(executor.dispatch(bindings, threadCount));

        check(counter[0] == (std::uint32_t) threadCount);

        for (auto index = 0; index < threadCount; ++index)
        {
            auto at = (std::size_t) index;
            auto x = inputAt(index);

            check(first[at] == x, "first " + std::to_string(index));
            check(second[at] == (x > 0.5f ? sentinel : x * 2.f),
                  "second " + std::to_string(index));
            check(third[at] == x + 1.f, "third " + std::to_string(index));
        }
    }
};

auto tValues = test("Function/structsHalvesAndMatricesPassByValue") = []
{
    auto kernel = ValueKernel {};

    for (const auto& options: batchings)
    {
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto input = StructBufferData<Body> {threadCount};
        auto bodies = StructBufferData<Body> {threadCount};
        auto halves = std::array<std::uint16_t, threadCount> {};
        auto halfOutput = filledWith(std::uint16_t {0xffffu});
        auto turns = filledWith(sentinel);

        for (auto i = 0; i < threadCount; ++i)
        {
            auto f = (float) i;
            input.set<&Body::position>(i, {f, -f, 0.5f});
            input.set<&Body::mass>(i, 2.f + f);
            halves[(std::size_t) i] = narrowToHalf(inputAt(i));
        }

        auto bindings = Bindings {};
        check(
            bindings.set(kernel.input, std::span<const std::byte> {input.bytes()}));
        check(bindings.set(kernel.bodies, bodies.bytes()));
        bindings.set(kernel.halves, std::span<const std::uint16_t> {halves});
        bindings.set(kernel.halfOutput, std::span<std::uint16_t> {halfOutput});
        bindings.set(kernel.turns, std::span<float> {turns});
        check(executor.dispatch(bindings, threadCount));

        for (auto i = 0; i < threadCount; ++i)
        {
            auto at = (std::size_t) i;
            auto f = (float) i;
            auto position = bodies.get<&Body::position>(i);
            auto h = widenHalf(halves[at]);
            auto k = widenHalf(narrowToHalf(f));
            auto product = widenHalf(narrowToHalf(h * h));

            check(position[0] == f * f && position[1] == -f * f
                      && position[2] == 0.5f * f,
                  "position " + std::to_string(i));
            check(bodies.get<&Body::mass>(i) == 2.f + f + f,
                  "mass " + std::to_string(i));
            check(halfOutput[at] == narrowToHalf(product + k),
                  "half " + std::to_string(i));
            check(turns[at] == -1.f * 10.f + f, "turn " + std::to_string(i));
        }
    }
};

auto tNoAllocation = test("Function/aDispatchAllocatesNothing") = []
{
    auto kernel = SearchKernel {};
    auto executor = Executor {kernel};
    auto found = filledWith(uintSentinel);
    auto counts = filledWith(uintSentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.found, std::span<std::uint32_t> {found});
    bindings.set(kernel.counts, std::span<std::uint32_t> {counts});

    auto warmedUp = executor.dispatch(bindings, threadCount);

    ExecutorTests::allocationsCounted = 0;
    ExecutorTests::countingHeap = true;
    auto ran = executor.dispatch(bindings, threadCount);
    ExecutorTests::countingHeap = false;

    check(warmedUp);
    check(ran);
    check(ExecutorTests::allocationsCounted.load() == 0);
    check(found[7] == firstSquareAbove(2u, 21u));
};
} // namespace
} // namespace FunctionTests
