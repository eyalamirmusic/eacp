#include "CountingHeap.h"

#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <array>
#include <climits>
#include <cstdint>
#include <string>

// WGSL's switch, early return, break if and for loop on the CPU executor, lanes
// diverging through each: a switch whose lanes each take a different clause
// or the default, a return some lanes take - before a write, inside a loop and
// inside a switch - with the later writes skipped for those lanes alone, a
// continuing block that a continue still runs and whose break-if takes lanes
// out one by one, and a switch nested in a switch in a loop whose clauses
// break and continue that loop. Every number is worked out here by a plain C++
// loop from the thread's id, batched several groups to a run and one group at
// a time; and the realtime contract.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace ControlFlowTests
{
namespace
{
constexpr auto sentinel = 0xdeadbeefu;
constexpr auto threadCount = 300;

Vector<std::uint32_t> wordsOf(int count, std::uint32_t value)
{
    auto values = Vector<std::uint32_t> {};
    values.resize(count, value);
    return values;
}

// Seven clauses' worth of lanes: 0 alone, 1 and 4 together, 2 with a branch
// of its own, and the default for the rest.
struct SwitchKernel final : ComputeKernel
{
    SwitchKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto value = var(0u);

        auto first = [&] { value = 100u + id; };
        auto shared = [&] { value = 200u; };
        auto bump = [&] { value += 1u; };
        auto tripled = [&]
        {
            value = id * 3u;
            ifThen(id > 20u, bump);
        };
        auto rest = [&] { value = 900u + id % 7u; };

        switchOn(id % 7u,
                 caseOf(0, first),
                 caseOf({1, 4}, shared),
                 caseOf(2, tripled),
                 defaultCase(rest));

        auto signedValue = var(0);
        auto low = [&] { signedValue = -1; };
        auto never = [&] { signedValue = -2; };
        auto high = [&] { signedValue = 5; };

        switchOn(toInt(id) - 8,
                 caseOf({-8, -1}, low),
                 caseOf(INT_MIN, never),
                 caseOf(INT_MAX, never),
                 caseOf(5, high));

        write(output, id, value.get());
        write(signedOutput, id, toUInt(signedValue.get()));
    }

    Uniform<UIntOutputBuffer> output;
    Uniform<UIntOutputBuffer> signedOutput;
    EACP_SHADER(output, signedOutput)
};

std::uint32_t expectedSwitch(std::uint32_t id)
{
    switch (id % 7u)
    {
        case 0u:
            return 100u + id;
        case 1u:
        case 4u:
            return 200u;
        case 2u:
            return id * 3u + (id > 20u ? 1u : 0u);
        default:
            return 900u + id % 7u;
    }
}

std::uint32_t expectedSignedSwitch(std::uint32_t id)
{
    auto selector = (int) id - 8;

    if (selector == -8 || selector == -1)
        return (std::uint32_t) -1;

    return selector == 5 ? 5u : 0u;
}

// A return taken by every third lane before the second write, by the odd
// lanes inside a loop, and by the lanes a switch's clause sends out.
struct EarlyReturnKernel final : ComputeKernel
{
    EarlyReturnKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto leave = [&] { returnEarly(); };

        write(first, id, 1u);
        ifThen(id % 3u == 0u, leave);
        write(second, id, 2u);

        auto counter = var(0u);
        auto step = [&]
        {
            counter += 1u;
            ifThen(counter.get() == id % 5u + 1u && id % 2u == 1u, leave);
        };

        loop(counter.get() < 10u, step);
        write(third, id, counter.get());

        switchOn(id % 4u, caseOf(2, leave));
        write(fourth, id, 4u);
    }

    Uniform<UIntOutputBuffer> first;
    Uniform<UIntOutputBuffer> second;
    Uniform<UIntOutputBuffer> third;
    Uniform<UIntOutputBuffer> fourth;
    EACP_SHADER(first, second, third, fourth)
};

// Totals the counters below 20 that are not multiples of three, the update and
// a break-if on the total in the continuing block - which the continue past
// the multiples still runs.
struct BreakIfKernel final : ComputeKernel
{
    BreakIfKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto counter = var(0u);
        auto total = var(0u);

        auto skip = [&] { continueLoop(); };
        auto body = [&]
        {
            ifThen(counter.get() % 3u == 0u, skip);
            total += counter.get();
        };

        auto update = [&]
        {
            counter += 1u;
            breakLoopIf(total.get() > id);
        };

        loop(counter.get() < 20u, body, update);
        write(totals, id, total.get());
        write(counters, id, counter.get());
    }

    Uniform<UIntOutputBuffer> totals;
    Uniform<UIntOutputBuffer> counters;
    EACP_SHADER(totals, counters)
};

struct BreakIfResult
{
    std::uint32_t total = 0;
    std::uint32_t counter = 0;
};

BreakIfResult expectedBreakIf(std::uint32_t id)
{
    auto result = BreakIfResult {};

    while (result.counter < 20u)
    {
        if (result.counter % 3u != 0u)
            result.total += result.counter;

        result.counter += 1u;

        if (result.total > id)
            break;
    }

    return result;
}

// The even numbers below id % 9 summed by a for loop whose body continues past
// the odd ones; a continue that skipped the update would never end.
struct ForLoopKernel final : ComputeKernel
{
    ForLoopKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto counter = var(0u);
        auto total = var(0u);

        auto update = [&] { counter += 1u; };
        auto skip = [&] { continueLoop(); };
        auto body = [&]
        {
            ifThen(counter.get() % 2u == 1u, skip);
            total += counter.get();
        };

        forLoop(counter.get() < id % 9u, update, body);
        write(output, id, total.get());
    }

    Uniform<UIntOutputBuffer> output;
    EACP_SHADER(output)
};

std::uint32_t expectedForLoop(std::uint32_t id)
{
    auto total = 0u;

    for (auto counter = 0u; counter < id % 9u; ++counter)
        if (counter % 2u == 0u)
            total += counter;

    return total;
}

// A counter walked to twelve whose every step a switch picks: on a multiple of
// four an inner switch on the thread breaks, continues or adds, on a remainder
// of one the counter is added, on two the loop breaks past the thread's id,
// and on three it continues. Every step not continued adds one.
struct NestedSwitchKernel final : ComputeKernel
{
    NestedSwitchKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto counter = var(0u);
        auto total = var(0u);

        auto stop = [&] { breakLoop(); };
        auto skip = [&] { continueLoop(); };
        auto addLots = [&] { total += 1000u; };
        auto inner = [&]
        {
            switchOn(
                id % 3u, caseOf(0, stop), caseOf(1, skip), defaultCase(addLots));
        };
        auto addCounter = [&] { total += counter.get(); };
        auto maybeStop = [&] { ifThen(counter.get() > id % 11u, stop); };

        auto step = [&]
        {
            counter += 1u;
            switchOn(counter.get() % 4u,
                     caseOf(0, inner),
                     caseOf(1, addCounter),
                     caseOf(2, maybeStop),
                     defaultCase(skip));
            total += 1u;
        };

        loop(counter.get() < 12u, step);
        write(totals, id, total.get());
        write(counters, id, counter.get());
    }

    Uniform<UIntOutputBuffer> totals;
    Uniform<UIntOutputBuffer> counters;
    EACP_SHADER(totals, counters)
};

BreakIfResult expectedNestedSwitch(std::uint32_t id)
{
    auto result = BreakIfResult {};

    while (result.counter < 12u)
    {
        result.counter += 1u;
        auto remainder = result.counter % 4u;

        if (remainder == 0u)
        {
            if (id % 3u == 0u)
                break;

            if (id % 3u == 1u)
                continue;

            result.total += 1000u;
        }
        else if (remainder == 1u)
        {
            result.total += result.counter;
        }
        else if (remainder == 2u)
        {
            if (result.counter > id % 11u)
                break;
        }
        else
        {
            continue;
        }

        result.total += 1u;
    }

    return result;
}

const auto batchings = std::array {PlanOptions {}, PlanOptions {1}};
} // namespace

auto tSwitch = test("ControlFlow/eachLaneTakesItsOwnClauseOrTheDefault") = []
{
    for (const auto& options: batchings)
    {
        auto kernel = SwitchKernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto output = wordsOf(threadCount, sentinel);
        auto signedOutput = wordsOf(threadCount, sentinel);

        auto bindings = Bindings {};
        bindings.set(kernel.output, output);
        bindings.set(kernel.signedOutput, signedOutput);
        check(executor.dispatch(bindings, threadCount));

        auto mismatches = 0;

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            mismatches += output[(int) id] != expectedSwitch(id);
            mismatches += signedOutput[(int) id] != expectedSignedSwitch(id);
        }

        check(mismatches == 0);
    }
};

auto tEarlyReturn =
    test("ControlFlow/aReturnSkipsTheLaterWritesOfItsLanesAlone") = []
{
    for (const auto& options: batchings)
    {
        auto kernel = EarlyReturnKernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto first = wordsOf(threadCount, sentinel);
        auto second = wordsOf(threadCount, sentinel);
        auto third = wordsOf(threadCount, sentinel);
        auto fourth = wordsOf(threadCount, sentinel);

        auto bindings = Bindings {};
        bindings.set(kernel.first, first);
        bindings.set(kernel.second, second);
        bindings.set(kernel.third, third);
        bindings.set(kernel.fourth, fourth);
        check(executor.dispatch(bindings, threadCount));

        auto mismatches = 0;

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            auto at = (int) id;
            auto pastFirst = id % 3u != 0u;
            auto pastLoop = pastFirst && id % 2u == 0u;
            auto pastSwitch = pastLoop && id % 4u != 2u;

            mismatches += first[at] != 1u;
            mismatches += second[at] != (pastFirst ? 2u : sentinel);
            mismatches += third[at] != (pastLoop ? 10u : sentinel);
            mismatches += fourth[at] != (pastSwitch ? 4u : sentinel);
        }

        check(mismatches == 0);
    }
};

auto tBreakIf = test("ControlFlow/breakIfEndsEachLaneAfterItsContinuing") = []
{
    for (const auto& options: batchings)
    {
        auto kernel = BreakIfKernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto totals = wordsOf(threadCount, sentinel);
        auto counters = wordsOf(threadCount, sentinel);

        auto bindings = Bindings {};
        bindings.set(kernel.totals, totals);
        bindings.set(kernel.counters, counters);
        check(executor.dispatch(bindings, threadCount));

        auto mismatches = 0;

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            auto expected = expectedBreakIf(id);
            mismatches += totals[(int) id] != expected.total;
            mismatches += counters[(int) id] != expected.counter;
        }

        check(mismatches == 0);
    }
};

auto tForLoop = test("ControlFlow/aContinueInAForLoopStillRunsTheUpdate") = []
{
    auto kernel = ForLoopKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = wordsOf(threadCount, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, threadCount));

    auto mismatches = 0;

    for (auto id = 0u; id < (unsigned) threadCount; ++id)
        mismatches += output[(int) id] != expectedForLoop(id);

    check(mismatches == 0);
};

auto tNestedSwitch =
    test("ControlFlow/clausesOfANestedSwitchBreakAndContinueTheLoop") = []
{
    for (const auto& options: batchings)
    {
        auto kernel = NestedSwitchKernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto totals = wordsOf(threadCount, sentinel);
        auto counters = wordsOf(threadCount, sentinel);

        auto bindings = Bindings {};
        bindings.set(kernel.totals, totals);
        bindings.set(kernel.counters, counters);
        check(executor.dispatch(bindings, threadCount));

        auto mismatches = 0;

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            auto expected = expectedNestedSwitch(id);
            mismatches += totals[(int) id] != expected.total;
            mismatches += counters[(int) id] != expected.counter;
        }

        check(mismatches == 0);
    }
};

auto tDiscardRefused = test("ControlFlow/aDiscardIsRefusedByName") = []
{
    auto graph = ShaderGraph {};
    auto output = graph.addStorageBuffer(BufferAccess::Write, ValueType::UInt);
    auto thread = graph.addThreadId();
    graph.addDiscard();
    graph.addStore(output, thread, thread);

    auto executor = Executor {graph};
    check(!executor.isValid());
    check(executor.reason().find("Discard") != std::string::npos, executor.reason());
};

auto tNoAllocation = test("ControlFlow/aDispatchAfterTheFirstAllocatesNothing") = []
{
    auto nested = NestedSwitchKernel {};
    auto early = EarlyReturnKernel {};
    auto breakIf = BreakIfKernel {};
    auto nestedExecutor = Executor {nested};
    auto earlyExecutor = Executor {early};
    auto breakIfExecutor = Executor {breakIf};

    auto words = Array<Vector<std::uint32_t>, 8> {};

    for (auto& array: words)
        array = wordsOf(threadCount, 0u);

    auto nestedBindings = Bindings {};
    nestedBindings.set(nested.totals, words[0]);
    nestedBindings.set(nested.counters, words[1]);

    auto earlyBindings = Bindings {};
    earlyBindings.set(early.first, words[2]);
    earlyBindings.set(early.second, words[3]);
    earlyBindings.set(early.third, words[4]);
    earlyBindings.set(early.fourth, words[5]);

    auto breakIfBindings = Bindings {};
    breakIfBindings.set(breakIf.totals, words[6]);
    breakIfBindings.set(breakIf.counters, words[7]);

    auto dispatchAll = [&]
    {
        return nestedExecutor.dispatch(nestedBindings, threadCount)
               && earlyExecutor.dispatch(earlyBindings, threadCount)
               && breakIfExecutor.dispatch(breakIfBindings, threadCount);
    };

    auto warmedUp = dispatchAll();

    ExecutorTests::allocationsCounted = 0;
    ExecutorTests::countingHeap = true;
    auto ran = dispatchAll();
    ExecutorTests::countingHeap = false;

    check(warmedUp);
    check(ran);
    check(ExecutorTests::allocationsCounted.load() == 0);
};
} // namespace ControlFlowTests
