#include "CountingHeap.h"

#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstdint>
#include <span>
#include <string>
#include <thread>

// WGSL's atomics on the CPU executor: every read-modify-write on an
// AtomicBuffer, an IntAtomicBuffer and the two SharedAtomic forms, one element
// per thread so each result has one right answer, against references written
// here from the WGSL wording; signed max and min at the edges of the range;
// compare-exchange both storing and not; counters shared by a whole dispatch
// split over threads of the caller's, where only real atomics keep the count;
// and the realtime contract.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace AtomicOpsTests
{
namespace
{
constexpr auto groupWidth = 64;
constexpr auto perElementCount = 2 * groupWidth;

constexpr auto readModifyWrites = std::array {AtomicOperation::Add,
                                              AtomicOperation::Sub,
                                              AtomicOperation::Max,
                                              AtomicOperation::Min,
                                              AtomicOperation::And,
                                              AtomicOperation::Or,
                                              AtomicOperation::Xor,
                                              AtomicOperation::Exchange};

std::int32_t signedOf(std::uint32_t word)
{
    return std::bit_cast<std::int32_t>(word);
}

std::uint32_t wordOf(std::int32_t value)
{
    return std::bit_cast<std::uint32_t>(value);
}

// What WGSL says the element holds after the operation.
std::uint32_t referenceAfter(AtomicOperation operation,
                             bool isSigned,
                             std::uint32_t old,
                             std::uint32_t value)
{
    switch (operation)
    {
        case AtomicOperation::Add:
            return old + value;
        case AtomicOperation::Sub:
            return old - value;
        case AtomicOperation::Max:
            if (isSigned)
                return wordOf(std::max(signedOf(old), signedOf(value)));

            return std::max(old, value);
        case AtomicOperation::Min:
            if (isSigned)
                return wordOf(std::min(signedOf(old), signedOf(value)));

            return std::min(old, value);
        case AtomicOperation::And:
            return old & value;
        case AtomicOperation::Or:
            return old | value;
        case AtomicOperation::Xor:
            return old ^ value;
        case AtomicOperation::Exchange:
        case AtomicOperation::CompareExchange:
            return value;
    }

    return old;
}

const char* nameOf(AtomicOperation operation)
{
    constexpr auto names = std::array {"Add",
                                       "Sub",
                                       "Max",
                                       "Min",
                                       "And",
                                       "Or",
                                       "Xor",
                                       "Exchange",
                                       "CompareExchange"};
    return names[static_cast<std::size_t>(operation)];
}

// The edges first - the two ends of both ranges, -1 against 0, a wrap each way
// - then a spread of words, so every operation meets a sign bit set and clear.
struct Pairs
{
    Pairs()
    {
        constexpr auto edges = std::array<std::array<std::uint32_t, 2>, 16> {{
            {0u, 0u},
            {0u, 1u},
            {1u, 0u},
            {5u, 3u},
            {3u, 5u},
            {0xffffffffu, 0u},
            {0u, 0xffffffffu},
            {0x80000000u, 0x7fffffffu},
            {0x7fffffffu, 0x80000000u},
            {0x80000000u, 1u},
            {0x7fffffffu, 1u},
            {0x80000000u, 0xffffffffu},
            {0xfffffffbu, 0xfffffffdu},
            {0xfffffffdu, 0xfffffffbu},
            {0xdeadbeefu, 0x12345678u},
            {0xf0f0f0f0u, 0x0ff00ff0u},
        }};

        for (const auto& edge: edges)
        {
            initial.add(edge[0]);
            operand.add(edge[1]);
        }

        auto state = 0x9e3779b9u;

        while (initial.size() < perElementCount)
        {
            state = state * 1664525u + 1013904223u;
            initial.add(state);
            state = state * 1664525u + 1013904223u;
            operand.add(state);
        }
    }

    Vector<std::uint32_t> initial;
    Vector<std::uint32_t> operand;
};

Vector<std::int32_t> signedWords(const Vector<std::uint32_t>& words)
{
    auto values = Vector<std::int32_t> {};

    for (auto word: words)
        values.add(signedOf(word));

    return values;
}

Vector<std::uint32_t> wordsOf(int count, std::uint32_t value)
{
    auto words = Vector<std::uint32_t> {};
    words.resize(count, value);
    return words;
}

// One read-modify-write a thread on its own element of a buffer: the old
// value out, the buffer left holding the new one.
struct BufferOpKernel final : ComputeKernel
{
    BufferOpKernel(AtomicOperation operationToUse, bool isSignedToUse)
        : ComputeKernel({groupWidth})
        , operation(operationToUse)
        , isSigned(isSignedToUse)
    {
        compile();
    }

    template <AtomicStorage Target>
    AtomicValue<Target>
        apply(const Target& target, const UInt& at, const AtomicValue<Target>& value)
    {
        switch (operation)
        {
            case AtomicOperation::Add:
                return atomicAdd(target, at, value);
            case AtomicOperation::Sub:
                return atomicSub(target, at, value);
            case AtomicOperation::Max:
                return atomicMax(target, at, value);
            case AtomicOperation::Min:
                return atomicMin(target, at, value);
            case AtomicOperation::And:
                return atomicAnd(target, at, value);
            case AtomicOperation::Or:
                return atomicOr(target, at, value);
            case AtomicOperation::Xor:
                return atomicXor(target, at, value);
            default:
                return atomicExchange(target, at, value);
        }
    }

    void define() override
    {
        auto id = threadId();

        if (isSigned)
            write(olds, id, toUInt(apply(intCounters, id, toInt(operands[id]))));
        else
            write(olds, id, apply(counters, id, operands[id]));
    }

    AtomicOperation operation;
    bool isSigned;

    Uniform<UIntInputBuffer> operands;
    Uniform<UIntOutputBuffer> olds;
    Uniform<AtomicBuffer> counters;
    Uniform<IntAtomicBuffer> intCounters;
    EACP_SHADER(operands, olds, counters, intCounters)
};

// The same on a group's shared counters: set from `initial`, barrier, the
// operation on the thread's own element, barrier, and both values written out.
struct SharedOpKernel final : ComputeKernel
{
    SharedOpKernel(AtomicOperation operationToUse, bool isSignedToUse)
        : ComputeKernel({groupWidth})
        , operation(operationToUse)
        , isSigned(isSignedToUse)
    {
        compile();
    }

    template <AtomicStorage Target>
    AtomicValue<Target>
        apply(const Target& target, const UInt& at, const AtomicValue<Target>& value)
    {
        switch (operation)
        {
            case AtomicOperation::Add:
                return atomicAdd(target, at, value);
            case AtomicOperation::Sub:
                return atomicSub(target, at, value);
            case AtomicOperation::Max:
                return atomicMax(target, at, value);
            case AtomicOperation::Min:
                return atomicMin(target, at, value);
            case AtomicOperation::And:
                return atomicAnd(target, at, value);
            case AtomicOperation::Or:
                return atomicOr(target, at, value);
            case AtomicOperation::Xor:
                return atomicXor(target, at, value);
            default:
                return atomicExchange(target, at, value);
        }
    }

    template <typename T>
    void run(const SharedAtomic<T>& counters, const UInt& id, const UInt& lane)
    {
        auto asElement = [](const UInt& word)
        {
            if constexpr (std::same_as<T, Int>)
                return toInt(word);
            else
                return word;
        };

        auto asWord = [](const T& value)
        {
            if constexpr (std::same_as<T, Int>)
                return toUInt(value);
            else
                return value;
        };

        write(counters, lane, asElement(initial[id]));
        barrier();
        auto old = apply(counters, lane, asElement(operands[id]));
        barrier();
        write(olds, id, asWord(old));
        write(finals, id, asWord(counters.load(lane)));
    }

    void define() override
    {
        auto id = threadId();
        auto lane = localId();

        if (isSigned)
            run(sharedAtomic<Int>(groupWidth), id, lane);
        else
            run(sharedAtomic<UInt>(groupWidth), id, lane);
    }

    AtomicOperation operation;
    bool isSigned;

    Uniform<UIntInputBuffer> initial;
    Uniform<UIntInputBuffer> operands;
    Uniform<UIntOutputBuffer> olds;
    Uniform<UIntOutputBuffer> finals;
    EACP_SHADER(initial, operands, olds, finals)
};

// A compare-exchange a thread on its own element: even threads compare against
// what the element holds, odd ones against something else.
struct CompareExchangeKernel final : ComputeKernel
{
    explicit CompareExchangeKernel(bool sharedToUse)
        : ComputeKernel({groupWidth})
        , shared(sharedToUse)
    {
        compile();
    }

    void define() override
    {
        auto id = threadId();
        auto lane = localId();
        auto outcome = CompareExchangeResult<Int> {};

        if (shared)
        {
            auto counters = sharedAtomic<Int>(groupWidth);
            write(counters, lane, toInt(initial[id]));
            barrier();
            outcome = atomicCompareExchangeWeak(
                counters, lane, toInt(compares[id]), toInt(operands[id]));
            barrier();
            write(finals, id, toUInt(counters.load(lane)));
        }
        else
        {
            outcome = atomicCompareExchangeWeak(
                intCounters, id, toInt(compares[id]), toInt(operands[id]));
        }

        write(olds, id, toUInt(outcome.oldValue));
        write(exchanged, id, select(outcome.exchanged, 1u, 0u));
    }

    bool shared;

    Uniform<UIntInputBuffer> initial;
    Uniform<UIntInputBuffer> compares;
    Uniform<UIntInputBuffer> operands;
    Uniform<UIntOutputBuffer> olds;
    Uniform<UIntOutputBuffer> exchanged;
    Uniform<UIntOutputBuffer> finals;
    Uniform<IntAtomicBuffer> intCounters;
    EACP_SHADER(initial, compares, operands, olds, exchanged, finals, intCounters)
};

constexpr auto unsignedCounters = 7;
constexpr auto signedCounters = 4;

std::uint32_t hashOf(std::uint32_t i)
{
    return (i * 2654435761u) ^ (i >> 7u);
}

// Every thread of a dispatch at the same few counters, each with an operation
// whose result is the same in every order the threads can run in.
struct ContendedKernel final : ComputeKernel
{
    ContendedKernel()
        : ComputeKernel({groupWidth})
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        auto hash = (i * 2654435761u) ^ (i >> 7u);

        atomicAdd(counters, 0u, 1u);
        atomicSub(counters, 1u, i & 7u);
        atomicMax(counters, 2u, hash);
        atomicMin(counters, 3u, hash);
        atomicAnd(counters, 4u, hash | 0x81818181u);
        atomicOr(counters, 5u, hash & 0x11111111u);
        atomicXor(counters, 6u, hash);

        atomicAdd(intCounters, 0u, toInt(i) - 2000);
        atomicMax(intCounters, 1u, toInt(hash));
        atomicMin(intCounters, 2u, toInt(hash));
        atomicSub(intCounters, 3u, toInt(i & 15u) - 7);
    }

    Uniform<AtomicBuffer> counters;
    Uniform<IntAtomicBuffer> intCounters;
    EACP_SHADER(counters, intCounters)
};

struct ContendedResult
{
    Vector<std::uint32_t> counters;
    Vector<std::int32_t> intCounters;
};

ContendedResult contendedStart()
{
    auto start = ContendedResult {};
    start.counters = {0u, 0u, 0u, 0xffffffffu, 0xffffffffu, 0u, 0x5a5a5a5au};
    start.intCounters = {0, INT_MIN, INT_MAX, 100};
    return start;
}

ContendedResult contendedReference(int count)
{
    auto result = contendedStart();
    auto& words = result.counters;
    auto& ints = result.intCounters;

    for (auto i = 0u; i < (std::uint32_t) count; ++i)
    {
        auto hash = hashOf(i);
        words[0] += 1u;
        words[1] -= i & 7u;
        words[2] = std::max(words[2], hash);
        words[3] = std::min(words[3], hash);
        words[4] &= hash | 0x81818181u;
        words[5] |= hash & 0x11111111u;
        words[6] ^= hash;

        ints[0] = signedOf(wordOf(ints[0]) + wordOf(signedOf(i) - 2000));
        ints[1] = std::max(ints[1], signedOf(hash));
        ints[2] = std::min(ints[2], signedOf(hash));
        ints[3] = signedOf(wordOf(ints[3]) - wordOf(signedOf(i & 15u) - 7));
    }

    return result;
}

// A ticket counter built from compare-exchange alone, retried until it stores:
// whatever the interleaving, the tickets are 0..count-1 once each.
struct CasCounterKernel final : ComputeKernel
{
    CasCounterKernel()
        : ComputeKernel({groupWidth})
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        auto done = var(false);
        auto ticket = var(0u);

        auto attempt = [&]
        {
            auto seen = counter.load(0u);
            auto outcome = atomicCompareExchangeWeak(counter, 0u, seen, seen + 1u);

            auto take = [&]
            {
                ticket = outcome.oldValue;
                done = true;
            };

            ifThen(outcome.exchanged, take);
        };

        loop(!done.get(), attempt);
        write(tickets, i, ticket.get());
    }

    Uniform<AtomicBuffer> counter;
    Uniform<UIntOutputBuffer> tickets;
    EACP_SHADER(counter, tickets)
};

// Each thread swaps its own number in and keeps what it took out.
struct ExchangeKernel final : ComputeKernel
{
    ExchangeKernel()
        : ComputeKernel({groupWidth})
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        write(olds, i, atomicExchange(slot, 0u, i + 1u));
    }

    Uniform<AtomicBuffer> slot;
    Uniform<UIntOutputBuffer> olds;
    EACP_SHADER(slot, olds)
};

// The groups of a dispatch split over this many threads of the caller's, each
// in a workspace of its own.
constexpr auto splitThreads = 8;

bool dispatchOnThreads(const Executor& executor, const PreparedDispatch& prepared)
{
    auto failures = std::atomic<int> {0};
    auto threads = std::array<std::thread, splitThreads> {};
    auto groups = prepared.groupCount();

    for (auto which = 0; which < splitThreads; ++which)
    {
        auto first = groups * which / splitThreads;
        auto last = groups * (which + 1) / splitThreads;

        auto runShare = [&executor, &prepared, &failures, first, last]
        {
            auto workspace = Workspace {executor.plan()};

            if (!executor.dispatchGroups(prepared, first, last - first, workspace))
                failures.fetch_add(1);
        };

        threads[static_cast<std::size_t>(which)] = std::thread {runShare};
    }

    for (auto& thread: threads)
        thread.join();

    return failures.load() == 0;
}

void expectPermutation(Vector<std::uint32_t> values, std::uint32_t first)
{
    std::sort(values.begin(), values.end());
    auto inOrder = 0;

    for (auto i = 0; i < values.size(); ++i)
        inOrder += values[i] == first + (std::uint32_t) i ? 1 : 0;

    check(inOrder == values.size());
}

struct SharedHistogramKernel final : ComputeKernel
{
    SharedHistogramKernel()
        : ComputeKernel({groupWidth})
    {
        compile();
    }

    void define() override
    {
        auto id = threadId();
        auto lane = localId();
        auto bins = sharedAtomic<UInt>(4);
        auto extremes = sharedAtomic<Int>(2);

        auto clear = [&]
        {
            for (auto bin = 0u; bin < 4u; ++bin)
                write(bins, bin, 0u);

            write(extremes, 0u, INT_MAX);
            write(extremes, 1u, INT_MIN);
        };

        ifThen(lane == 0u, clear);
        barrier();

        auto value = toInt(values[id]);
        atomicAdd(bins, values[id] & 3u, 1u);
        atomicMin(extremes, 0u, value);
        atomicMax(extremes, 1u, value);
        barrier();

        auto base = groupId() * 6u;
        auto publish = [&]
        {
            for (auto bin = 0u; bin < 4u; ++bin)
                write(totals, base + bin, bins.load(bin));

            write(totals, base + 4u, toUInt(extremes.load(0u)));
            write(totals, base + 5u, toUInt(extremes.load(1u)));
        };

        ifThen(lane == 0u, publish);
    }

    Uniform<UIntInputBuffer> values;
    Uniform<UIntOutputBuffer> totals;
    EACP_SHADER(values, totals)
};
} // namespace

auto tBufferOps = test("AtomicOps/everyOperationOnABufferYieldsTheOldValue") = []
{
    auto pairs = Pairs {};

    for (auto isSigned: {false, true})
    {
        for (auto operation: readModifyWrites)
        {
            auto kernel = BufferOpKernel {operation, isSigned};
            auto executor = Executor {kernel};
            check(executor.isValid(), executor.reason());

            auto counters = pairs.initial;
            auto intCounters = signedWords(pairs.initial);
            auto olds = wordsOf(perElementCount, 0xdeadbeefu);

            auto bindings = Bindings {};
            bindings.set(kernel.operands, pairs.operand);
            bindings.set(kernel.olds, olds);
            bindings.set(kernel.counters, counters);
            bindings.set(kernel.intCounters, intCounters);
            check(executor.dispatch(bindings, perElementCount));

            auto agreeing = 0;

            for (auto i = 0; i < perElementCount; ++i)
            {
                auto after = isSigned ? wordOf(intCounters[i]) : counters[i];
                auto expected = referenceAfter(
                    operation, isSigned, pairs.initial[i], pairs.operand[i]);

                if (olds[i] == pairs.initial[i] && after == expected)
                    ++agreeing;
            }

            check(agreeing == perElementCount,
                  std::string(nameOf(operation)) + (isSigned ? " i32" : " u32"));
        }
    }
};

auto tSharedOps =
    test("AtomicOps/everyOperationOnSharedMemoryYieldsTheOldValue") = []
{
    auto pairs = Pairs {};

    for (auto isSigned: {false, true})
    {
        for (auto operation: readModifyWrites)
        {
            auto kernel = SharedOpKernel {operation, isSigned};
            auto executor = Executor {kernel};
            check(executor.isValid(), executor.reason());

            auto olds = wordsOf(perElementCount, 0xdeadbeefu);
            auto finals = wordsOf(perElementCount, 0xdeadbeefu);

            auto bindings = Bindings {};
            bindings.set(kernel.initial, pairs.initial);
            bindings.set(kernel.operands, pairs.operand);
            bindings.set(kernel.olds, olds);
            bindings.set(kernel.finals, finals);
            check(executor.dispatch(bindings, perElementCount));

            auto agreeing = 0;

            for (auto i = 0; i < perElementCount; ++i)
            {
                auto expected = referenceAfter(
                    operation, isSigned, pairs.initial[i], pairs.operand[i]);

                if (olds[i] == pairs.initial[i] && finals[i] == expected)
                    ++agreeing;
            }

            check(agreeing == perElementCount,
                  std::string(nameOf(operation)) + (isSigned ? " i32" : " u32"));
        }
    }
};

auto tSignedEdges = test("AtomicOps/signedMaxAndMinOrderAsIntegers") = []
{
    auto initial = Vector<std::uint32_t> {
        wordOf(INT_MIN), wordOf(INT_MAX), wordOf(-1), 0u, wordOf(-7)};
    auto operand = Vector<std::uint32_t> {
        wordOf(INT_MAX), wordOf(INT_MIN), 0u, wordOf(-1), wordOf(-3)};

    struct Expected
    {
        AtomicOperation operation;
        bool isSigned;
        std::array<std::uint32_t, 5> after;
    };

    auto expectations = std::array {
        Expected {AtomicOperation::Max,
                  true,
                  {wordOf(INT_MAX), wordOf(INT_MAX), 0u, 0u, wordOf(-3)}},
        Expected {
            AtomicOperation::Min,
            true,
            {wordOf(INT_MIN), wordOf(INT_MIN), wordOf(-1), wordOf(-1), wordOf(-7)}},
        Expected {AtomicOperation::Max,
                  false,
                  {0x80000000u, 0x80000000u, 0xffffffffu, 0xffffffffu, wordOf(-3)}},
        Expected {AtomicOperation::Min,
                  false,
                  {0x7fffffffu, 0x7fffffffu, 0u, 0u, wordOf(-7)}},
    };

    for (const auto& expected: expectations)
    {
        auto kernel = BufferOpKernel {expected.operation, expected.isSigned};
        auto executor = Executor {kernel};

        auto counters = initial;
        auto intCounters = signedWords(initial);
        auto olds = wordsOf(initial.size(), 0u);

        auto bindings = Bindings {};
        bindings.set(kernel.operands, operand);
        bindings.set(kernel.olds, olds);
        bindings.set(kernel.counters, counters);
        bindings.set(kernel.intCounters, intCounters);
        check(executor.dispatch(bindings, initial.size()));

        for (auto i = 0; i < initial.size(); ++i)
        {
            auto after = expected.isSigned ? wordOf(intCounters[i]) : counters[i];
            check(after == expected.after[static_cast<std::size_t>(i)]);
            check(olds[i] == initial[i]);
        }
    }
};

auto tCompareExchange =
    test("AtomicOps/compareExchangeStoresOnlyWhereTheComparandMatches") = []
{
    auto pairs = Pairs {};
    auto compares = Vector<std::uint32_t> {};

    for (auto i = 0; i < perElementCount; ++i)
        compares.add(i % 2 == 0 ? pairs.initial[i] : pairs.initial[i] ^ 0x10u);

    for (auto shared: {false, true})
    {
        auto kernel = CompareExchangeKernel {shared};
        auto executor = Executor {kernel};
        check(executor.isValid(), executor.reason());

        auto intCounters = signedWords(pairs.initial);
        auto olds = wordsOf(perElementCount, 0xdeadbeefu);
        auto exchanged = wordsOf(perElementCount, 0xdeadbeefu);
        auto finals = wordsOf(perElementCount, 0xdeadbeefu);

        auto bindings = Bindings {};
        bindings.set(kernel.initial, pairs.initial);
        bindings.set(kernel.compares, compares);
        bindings.set(kernel.operands, pairs.operand);
        bindings.set(kernel.olds, olds);
        bindings.set(kernel.exchanged, exchanged);
        bindings.set(kernel.finals, finals);
        bindings.set(kernel.intCounters, intCounters);
        check(executor.dispatch(bindings, perElementCount));

        auto agreeing = 0;

        for (auto i = 0; i < perElementCount; ++i)
        {
            auto stored = i % 2 == 0;
            auto after = shared ? finals[i] : wordOf(intCounters[i]);
            auto expected = stored ? pairs.operand[i] : pairs.initial[i];

            if (olds[i] == pairs.initial[i] && exchanged[i] == (stored ? 1u : 0u)
                && after == expected)
                ++agreeing;
        }

        check(agreeing == perElementCount, shared ? "shared" : "buffer");
    }
};

auto tSharedHistogram = test("AtomicOps/sharedCountersSumTheGroupAfterABarrier") = []
{
    constexpr auto groups = 3;
    auto kernel = SharedHistogramKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto values = Vector<std::uint32_t> {};
    auto state = 12345u;

    for (auto i = 0; i < groups * groupWidth; ++i)
    {
        state = state * 1664525u + 1013904223u;
        values.add(state);
    }

    auto totals = wordsOf(groups * 6, 0xdeadbeefu);
    auto bindings = Bindings {};
    bindings.set(kernel.values, values);
    bindings.set(kernel.totals, totals);
    check(executor.dispatch(bindings, groups * groupWidth));

    for (auto group = 0; group < groups; ++group)
    {
        auto bins = std::array<std::uint32_t, 4> {};
        auto low = INT_MAX;
        auto high = INT_MIN;

        for (auto lane = 0; lane < groupWidth; ++lane)
        {
            auto value = values[group * groupWidth + lane];
            ++bins[value & 3u];
            low = std::min(low, signedOf(value));
            high = std::max(high, signedOf(value));
        }

        for (auto bin = 0; bin < 4; ++bin)
            check(totals[group * 6 + bin] == bins[static_cast<std::size_t>(bin)]);

        check(totals[group * 6 + 4] == wordOf(low));
        check(totals[group * 6 + 5] == wordOf(high));
    }
};

auto tContended = test("AtomicOps/aContendedDispatchSplitOverThreadsKeepsCount") = []
{
    constexpr auto count = 1 << 16;
    auto kernel = ContendedKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto expected = contendedReference(count);

    for (auto split: {false, true})
    {
        auto start = contendedStart();
        auto bindings = Bindings {};
        bindings.set(kernel.counters, start.counters);
        bindings.set(kernel.intCounters, start.intCounters);

        if (split)
        {
            auto prepared = executor.prepareDispatch(bindings, count);
            check(prepared.isValid());
            check(dispatchOnThreads(executor, prepared));
        }
        else
        {
            check(executor.dispatch(bindings, count));
        }

        check(start.counters == expected.counters, split ? "split u32" : "one u32");
        check(start.intCounters == expected.intCounters,
              split ? "split i32" : "one i32");
    }
};

auto tCasCounter =
    test("AtomicOps/aCompareExchangeCounterHandsOutEveryTicketOnce") = []
{
    constexpr auto count = 4096;
    auto kernel = CasCounterKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    for (auto split: {false, true})
    {
        auto counter = wordsOf(1, 0u);
        auto tickets = wordsOf(count, 0xdeadbeefu);

        auto bindings = Bindings {};
        bindings.set(kernel.counter, counter);
        bindings.set(kernel.tickets, tickets);

        if (split)
        {
            auto prepared = executor.prepareDispatch(bindings, count);
            check(dispatchOnThreads(executor, prepared));
        }
        else
        {
            check(executor.dispatch(bindings, count));
        }

        check(counter[0] == (std::uint32_t) count);
        expectPermutation(tickets, 0u);
    }
};

auto tExchange = test("AtomicOps/exchangedValuesAreEachTakenOutOnce") = []
{
    constexpr auto count = 4096;
    auto kernel = ExchangeKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    for (auto split: {false, true})
    {
        auto slot = wordsOf(1, 0u);
        auto olds = wordsOf(count, 0xdeadbeefu);

        auto bindings = Bindings {};
        bindings.set(kernel.slot, slot);
        bindings.set(kernel.olds, olds);

        if (split)
        {
            auto prepared = executor.prepareDispatch(bindings, count);
            check(dispatchOnThreads(executor, prepared));
        }
        else
        {
            check(executor.dispatch(bindings, count));
        }

        auto seen = olds;
        seen.add(slot[0]);
        expectPermutation(seen, 0u);
    }
};

auto tOutOfRange = test("AtomicOps/anElementOutOfRangeIsLeftAloneAndYieldsZero") = []
{
    auto kernel = CompareExchangeKernel {false};
    auto executor = Executor {kernel};

    auto inputs = wordsOf(4, 7u);
    auto intCounters = Vector<std::int32_t> {7, 7};
    auto olds = wordsOf(4, 0xdeadbeefu);
    auto exchanged = wordsOf(4, 0xdeadbeefu);
    auto finals = wordsOf(4, 0u);

    auto bindings = Bindings {};
    bindings.set(kernel.initial, inputs);
    bindings.set(kernel.compares, inputs);
    bindings.set(kernel.operands, inputs);
    bindings.set(kernel.olds, olds);
    bindings.set(kernel.exchanged, exchanged);
    bindings.set(kernel.finals, finals);
    bindings.set(kernel.intCounters, intCounters);
    check(executor.dispatch(bindings, 4));

    check(olds[0] == 7u && olds[1] == 7u && olds[2] == 0u && olds[3] == 0u);
    check(exchanged[0] == 1u && exchanged[1] == 1u);
    check(exchanged[2] == 0u && exchanged[3] == 0u);
};

auto tRefusals = test("AtomicOps/anAtomicOnPlainSharedMemoryIsRefused") = []
{
    auto graph = ShaderGraph {};
    auto output = graph.addStorageBuffer(BufferAccess::Write, ValueType::UInt);
    auto plain = graph.addSharedArray(ValueType::UInt, 4);
    auto index = graph.addThreadId();
    auto previous = graph.addAtomic(AtomicOperation::Max,
                                    MemorySpace::Shared,
                                    plain,
                                    graph.addUIntConstant(0u),
                                    index);
    graph.addStore(output, index, graph.addVarRead(previous.previous));

    auto executor = Executor {graph};
    check(!executor.isValid());
    check(executor.reason().find("not atomic") != std::string::npos);
};

auto tNoAllocation = test("AtomicOps/aDispatchAfterTheFirstAllocatesNothing") = []
{
    constexpr auto count = 1000;
    auto contended = ContendedKernel {};
    auto shared = SharedOpKernel {AtomicOperation::Max, true};
    auto contendedExecutor = Executor {contended};
    auto sharedExecutor = Executor {shared};

    auto start = contendedStart();
    auto pairs = Pairs {};
    auto olds = wordsOf(perElementCount, 0u);
    auto finals = wordsOf(perElementCount, 0u);

    auto contendedBindings = Bindings {};
    contendedBindings.set(contended.counters, start.counters);
    contendedBindings.set(contended.intCounters, start.intCounters);

    auto sharedBindings = Bindings {};
    sharedBindings.set(shared.initial, pairs.initial);
    sharedBindings.set(shared.operands, pairs.operand);
    sharedBindings.set(shared.olds, olds);
    sharedBindings.set(shared.finals, finals);

    auto warmedUp = contendedExecutor.dispatch(contendedBindings, count)
                    && sharedExecutor.dispatch(sharedBindings, perElementCount);

    ExecutorTests::allocationsCounted = 0;
    ExecutorTests::countingHeap = true;
    auto ran = contendedExecutor.dispatch(contendedBindings, count)
               && sharedExecutor.dispatch(sharedBindings, perElementCount);
    ExecutorTests::countingHeap = false;

    check(warmedUp);
    check(ran);
    check(ExecutorTests::allocationsCounted.load() == 0);
};
} // namespace AtomicOpsTests
