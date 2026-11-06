#include "CpuCrossCheck.h"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstdint>
#include <string>

// WGSL's atomics on the device, each kernel run on the CPU executor too. One
// element a thread has one right answer, so those backends agree word for
// word; where every thread of a dispatch meets at the same counters, only what
// holds in every interleaving is checked - the final value of an operation
// that commutes, every ticket a compare-exchange loop hands out once, every
// exchanged value taken out once.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace AtomicOpsDeviceTests
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
            return isSigned ? wordOf(std::max(signedOf(old), signedOf(value)))
                            : std::max(old, value);
        case AtomicOperation::Min:
            return isSigned ? wordOf(std::min(signedOf(old), signedOf(value)))
                            : std::min(old, value);
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

std::string
    labelOf(AtomicOperation operation, bool isSigned, const Readback& readback)
{
    constexpr auto names =
        std::array {"Add", "Sub", "Max", "Min", "And", "Or", "Xor", "Exchange"};
    return std::string(readback.name()) + " "
           + names[static_cast<std::size_t>(operation)]
           + (isSigned ? " i32" : " u32");
}

Vector<std::uint32_t> wordsOf(const Vector<std::int32_t>& values)
{
    auto words = Vector<std::uint32_t> {};

    for (auto value: values)
        words.add(wordOf(value));

    return words;
}

// The ends of both ranges against each other first, then a spread of words.
struct Pairs
{
    Pairs()
    {
        constexpr auto edges = std::array<std::array<std::uint32_t, 2>, 12> {{
            {0u, 1u},
            {5u, 3u},
            {3u, 5u},
            {0xffffffffu, 0u},
            {0u, 0xffffffffu},
            {0x80000000u, 0x7fffffffu},
            {0x7fffffffu, 0x80000000u},
            {0x80000000u, 1u},
            {0x7fffffffu, 1u},
            {0xfffffffbu, 0xfffffffdu},
            {0xfffffffdu, 0xfffffffbu},
            {0xdeadbeefu, 0x12345678u},
        }};

        for (const auto& edge: edges)
        {
            initial.add(edge[0]);
            operand.add(edge[1]);
        }

        auto state = 0x2545f491u;

        while (initial.size() < perElementCount)
        {
            state = state * 1664525u + 1013904223u;
            initial.add(state);
            state = state * 1664525u + 1013904223u;
            operand.add(state);
        }
    }

    Vector<std::int32_t> signedInitial() const
    {
        auto values = Vector<std::int32_t> {};

        for (auto word: initial)
            values.add(signedOf(word));

        return values;
    }

    Vector<std::uint32_t> initial;
    Vector<std::uint32_t> operand;
};

struct PerElementKernel final : ComputeProgram
{
    PerElementKernel(AtomicOperation operationToUse,
                     bool sharedToUse,
                     bool isSignedToUse)
        : ComputeProgram({groupWidth})
        , operation(operationToUse)
        , shared(sharedToUse)
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

    // A thread's own element of a group's shared counters, set from `initial`
    // and read back into `finals` after the operation, between barriers.
    template <typename T>
    void onShared(const UInt& id)
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

        auto lane = localId();
        auto counters = sharedAtomic<T>(groupWidth);
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

        if (shared && isSigned)
            onShared<Int>(id);
        else if (shared)
            onShared<UInt>(id);
        else if (isSigned)
            write(olds, id, toUInt(apply(intCounters, id, toInt(operands[id]))));
        else
            write(olds, id, apply(counters, id, operands[id]));
    }

    AtomicOperation operation;
    bool shared;
    bool isSigned;

    Uniform<UIntInputBuffer> initial;
    Uniform<UIntInputBuffer> operands;
    Uniform<UIntOutputBuffer> olds;
    Uniform<UIntOutputBuffer> finals;
    Uniform<AtomicBuffer> counters;
    Uniform<IntAtomicBuffer> intCounters;
    EACP_SHADER(initial, operands, olds, finals, counters, intCounters)
};

// Each thread compares its own element with what it holds (even threads) or
// with something else (odd ones).
struct CompareExchangeKernel final : ComputeProgram
{
    CompareExchangeKernel(bool sharedToUse, bool isSignedToUse)
        : ComputeProgram({groupWidth})
        , shared(sharedToUse)
        , isSigned(isSignedToUse)
    {
        compile();
    }

    template <AtomicStorage Target>
    void swap(const Target& target, const UInt& id, const UInt& at)
    {
        auto asElement = [](const UInt& word)
        {
            if constexpr (std::same_as<AtomicValue<Target>, Int>)
                return toInt(word);
            else
                return word;
        };

        auto outcome = atomicCompareExchangeWeak(
            target, at, asElement(compares[id]), asElement(operands[id]));

        if constexpr (std::same_as<AtomicValue<Target>, Int>)
            write(olds, id, toUInt(outcome.oldValue));
        else
            write(olds, id, outcome.oldValue);

        write(exchanged, id, select(outcome.exchanged, 1u, 0u));
    }

    void define() override
    {
        auto id = threadId();

        if (!shared)
        {
            if (isSigned)
                swap(intCounters, id, id);
            else
                swap(counters, id, id);

            return;
        }

        auto lane = localId();
        auto group = sharedAtomic<Int>(groupWidth);
        write(group, lane, toInt(initial[id]));
        barrier();
        swap(group, id, lane);
        barrier();
        write(finals, id, toUInt(group.load(lane)));
    }

    bool shared;
    bool isSigned;

    Uniform<UIntInputBuffer> initial;
    Uniform<UIntInputBuffer> compares;
    Uniform<UIntInputBuffer> operands;
    Uniform<UIntOutputBuffer> olds;
    Uniform<UIntOutputBuffer> exchanged;
    Uniform<UIntOutputBuffer> finals;
    Uniform<AtomicBuffer> counters;
    Uniform<IntAtomicBuffer> intCounters;
    EACP_SHADER(
        initial, compares, operands, olds, exchanged, finals, counters, intCounters)
};

std::uint32_t hashOf(std::uint32_t i)
{
    return (i * 2654435761u) ^ (i >> 7u);
}

// Every thread at the same few counters, with operations that commute.
struct ContendedKernel final : ComputeProgram
{
    ContendedKernel()
        : ComputeProgram({groupWidth})
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

struct ContendedCounters
{
    Vector<std::uint32_t> words {
        0u, 0u, 0u, 0xffffffffu, 0xffffffffu, 0u, 0x5a5a5a5au};
    Vector<std::int32_t> ints {0, INT_MIN, INT_MAX, 100};
};

ContendedCounters contendedReference(int count)
{
    auto result = ContendedCounters {};
    auto& words = result.words;
    auto& ints = result.ints;

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

// A counter built from compare-exchange alone, each thread retrying until it
// stores - spurious failures of the weak form included.
struct CasCounterKernel final : ComputeProgram
{
    CasCounterKernel()
        : ComputeProgram({groupWidth})
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

struct ExchangeKernel final : ComputeProgram
{
    ExchangeKernel()
        : ComputeProgram({groupWidth})
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

// Per group: four bins counted and a signed low and high, in shared counters
// one thread zeroes before the first barrier, published by that thread after
// the second.
struct HistogramKernel final : ComputeProgram
{
    HistogramKernel()
        : ComputeProgram({groupWidth})
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

bool isPermutationFrom(Vector<std::uint32_t> values, std::uint32_t first)
{
    std::sort(values.begin(), values.end());

    for (auto i = 0; i < values.size(); ++i)
        if (values[i] != first + (std::uint32_t) i)
            return false;

    return true;
}
} // namespace

auto tPerElement =
    test("AtomicOpsDevice/everyOperationAgreesWithTheCpuElementForElement") = []
{
    auto pairs = Pairs {};

    for (auto shared: {false, true})
    {
        for (auto isSigned: {false, true})
        {
            for (auto operation: readModifyWrites)
            {
                auto kernel = PerElementKernel {operation, shared, isSigned};

                auto verify = [&](const Readback& readback)
                {
                    const auto& olds = readback.uints(kernel.olds);
                    auto afters = readback.uints(kernel.finals);

                    if (!shared)
                        afters = isSigned
                                     ? wordsOf(readback.ints(kernel.intCounters))
                                     : readback.uints(kernel.counters);

                    auto agreeing = 0;

                    for (auto i = 0; i < perElementCount; ++i)
                    {
                        auto expected = referenceAfter(
                            operation, isSigned, pairs.initial[i], pairs.operand[i]);

                        if (olds[i] == pairs.initial[i] && afters[i] == expected)
                            ++agreeing;
                    }

                    check(agreeing == perElementCount,
                          labelOf(operation, isSigned, readback)
                              + (shared ? " shared" : " buffer"));
                };

                CrossCheck {kernel}
                    .input(kernel.initial, pairs.initial)
                    .input(kernel.operands, pairs.operand)
                    .output(kernel.olds, perElementCount, 0xdeadbeefu)
                    .output(kernel.finals, perElementCount, 0xdeadbeefu)
                    .output(kernel.counters, pairs.initial)
                    .output(kernel.intCounters, pairs.signedInitial())
                    .agreeing()
                    .run(perElementCount, verify);
            }
        }
    }
};

// Weak on Metal: a compare-exchange may fail although the element equals the
// comparand. So this checks what holds either way - the old value is what
// the element held, a mismatch never stores, and the element afterwards is
// the operand exactly when the exchange says it stored.
auto tCompareExchange =
    test("AtomicOpsDevice/compareExchangeStoresOnlyWhereTheComparandMatches") = []
{
    auto pairs = Pairs {};
    auto compares = Vector<std::uint32_t> {};

    for (auto i = 0; i < perElementCount; ++i)
        compares.add(i % 2 == 0 ? pairs.initial[i] : pairs.initial[i] ^ 0x100u);

    struct Variant
    {
        bool shared;
        bool isSigned;
    };

    for (auto variant:
         {Variant {false, false}, Variant {false, true}, Variant {true, true}})
    {
        auto kernel = CompareExchangeKernel {variant.shared, variant.isSigned};

        auto verify = [&](const Readback& readback)
        {
            const auto& olds = readback.uints(kernel.olds);
            const auto& exchanged = readback.uints(kernel.exchanged);
            auto afters = readback.uints(kernel.finals);

            if (!variant.shared)
                afters = variant.isSigned
                             ? wordsOf(readback.ints(kernel.intCounters))
                             : readback.uints(kernel.counters);

            auto holding = 0;
            auto stored = 0;

            for (auto i = 0; i < perElementCount; ++i)
            {
                auto matches = i % 2 == 0;
                auto didStore = exchanged[i] == 1u;
                auto expected = didStore ? pairs.operand[i] : pairs.initial[i];

                if (olds[i] == pairs.initial[i] && afters[i] == expected
                    && (exchanged[i] <= 1u) && (matches || !didStore))
                    ++holding;

                stored += didStore ? 1 : 0;
            }

            check(holding == perElementCount, readback.name());
            check(stored > 0, readback.name());

            if (readback.backend == Backend::Cpu)
                check(stored == perElementCount / 2, "cpu: strong");
        };

        CrossCheck {kernel}
            .input(kernel.initial, pairs.initial)
            .input(kernel.compares, compares)
            .input(kernel.operands, pairs.operand)
            .output(kernel.olds, perElementCount, 0xdeadbeefu)
            .output(kernel.exchanged, perElementCount, 0xdeadbeefu)
            .output(kernel.finals, perElementCount, 0xdeadbeefu)
            .output(kernel.counters, pairs.initial)
            .output(kernel.intCounters, pairs.signedInitial())
            .run(perElementCount, verify);
    }
};

auto tContended =
    test("AtomicOpsDevice/contendedCountersEndWhereEveryOrderEnds") = []
{
    constexpr auto count = 1 << 14;
    auto kernel = ContendedKernel {};
    auto start = ContendedCounters {};
    auto expected = contendedReference(count);

    auto verify = [&](const Readback& readback)
    {
        check(readback.uints(kernel.counters) == expected.words, readback.name());
        check(readback.ints(kernel.intCounters) == expected.ints, readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.counters, start.words)
        .output(kernel.intCounters, start.ints)
        .agreeing()
        .run(count, verify);
};

auto tCasCounter =
    test("AtomicOpsDevice/aCompareExchangeCounterHandsOutEveryTicketOnce") = []
{
    constexpr auto count = 2048;
    auto kernel = CasCounterKernel {};

    auto verify = [&](const Readback& readback)
    {
        check(readback.uints(kernel.counter)[0] == (std::uint32_t) count,
              readback.name());
        check(isPermutationFrom(readback.uints(kernel.tickets), 0u),
              readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.counter, 1)
        .output(kernel.tickets, count, 0xdeadbeefu)
        .run(count, verify);
};

auto tExchange = test("AtomicOpsDevice/exchangedValuesAreEachTakenOutOnce") = []
{
    constexpr auto count = 4096;
    auto kernel = ExchangeKernel {};

    auto verify = [&](const Readback& readback)
    {
        auto seen = readback.uints(kernel.olds);
        seen.add(readback.uints(kernel.slot)[0]);
        check(isPermutationFrom(seen, 0u), readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.slot, 1)
        .output(kernel.olds, count, 0xdeadbeefu)
        .run(count, verify);
};

auto tHistogram = test("AtomicOpsDevice/sharedCountersSumTheGroupAfterABarrier") = []
{
    constexpr auto groups = 16;
    auto kernel = HistogramKernel {};

    auto values = Vector<std::uint32_t> {};
    auto state = 777u;

    for (auto i = 0; i < groups * groupWidth; ++i)
    {
        state = state * 1664525u + 1013904223u;
        values.add(state);
    }

    auto expected = Vector<std::uint32_t> {};

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

        for (auto bin: bins)
            expected.add(bin);

        expected.add(wordOf(low));
        expected.add(wordOf(high));
    }

    auto verify = [&](const Readback& readback)
    { check(readback.uints(kernel.totals) == expected, readback.name()); };

    CrossCheck {kernel}
        .input(kernel.values, values)
        .output(kernel.totals, groups * 6, 0xdeadbeefu)
        .agreeing()
        .run(groups * groupWidth, verify);
};
} // namespace AtomicOpsDeviceTests
