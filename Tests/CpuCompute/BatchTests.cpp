#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <utility>

// Wide batches: a kernel with no group-scope feature runs several consecutive
// x groups as one batch. Every kernel here runs at 1, 4 and 16 groups per
// batch and must give the same bits each time, and the bits its C++ twin gives.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace
{
constexpr auto sentinel = -1.f;
constexpr auto sentinelBits = 0xdeadbeefu;
constexpr auto batchWidths = std::array {1, 4, 16};

using Words = Vector<std::uint32_t>;

Vector<float> makeFloats(int count, float value)
{
    auto values = Vector<float> {};
    values.resize(count, value);
    return values;
}

Words makeUInts(int count, std::uint32_t value)
{
    auto values = Words {};
    values.resize(count, value);
    return values;
}

Vector<float> wobble(int count)
{
    auto values = Vector<float> {};

    for (auto k = 0; k < count; ++k)
        values.add((float) k * 0.37f - (float) (k % 7) * 1.5f);

    return values;
}

float elementOr(const Vector<float>& values, std::uint32_t index)
{
    return index < (std::uint32_t) values.size() ? values[(int) index] : 0.f;
}

Words bitsOf(const Vector<float>& values)
{
    auto bits = Words {};

    for (auto value: values)
        bits.add(std::bit_cast<std::uint32_t>(value));

    return bits;
}

Words joined(const Words& a, const Words& b)
{
    auto all = a;

    for (auto word: b)
        all.add(word);

    return all;
}

int groupLanes(const ComputeKernel& kernel)
{
    auto shape = kernel.graph().threadGroupShape();
    return shape.x * shape.y * shape.z;
}

constexpr auto unlimitedBytes = std::numeric_limits<std::size_t>::max();

PlanOptions batchesOf(const ComputeKernel& kernel, int groups)
{
    return {groupLanes(kernel) * groups, unlimitedBytes};
}

bool sameWords(const Words& a, const Words& b, int groups)
{
    if (a.size() != b.size())
        return false;

    for (auto i = 0; i < a.size(); ++i)
    {
        if (a[i] != b[i])
        {
            std::printf("    %d groups per batch: word %d is 0x%08x, not 0x%08x\n",
                        groups,
                        i,
                        a[i],
                        b[i]);
            return false;
        }
    }

    return true;
}

// Runs the kernel at every batch width through `run`, which dispatches and
// returns the words the dispatch left, and checks each against the twin's and
// against one group per batch.
template <typename Kernel, typename Run>
void checkEveryWidth(Kernel& kernel, const Words& twin, Run&& run)
{
    auto narrow = Words {};

    for (auto groups: batchWidths)
    {
        auto executor = Executor {kernel, batchesOf(kernel, groups)};
        check(executor.isValid(), executor.reason());
        check(executor.plan().groupsPerBatch() == groups);
        check(executor.plan().batchLanes() == groupLanes(kernel) * groups);

        auto words = run(executor);

        if (groups == 1)
            narrow = words;

        check(sameWords(words, narrow, groups));
        check(sameWords(words, twin, groups));
    }
}

// ------------------------------------------------------------------- streams

struct StreamKernel final : ComputeKernel
{
    StreamKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto sum = input[i] * gain + input[i + 1u];
        write(output, i, sum);
        write2(pairs, i, float2(sum, input[i * 3u]));
        write(spread, i * 3u + 1u, input.read2(i).y());
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<OutputBuffer> pairs;
    Uniform<OutputBuffer> spread;
    Uniform<Float> gain;

    EACP_SHADER(input, output, pairs, spread, gain)
};

constexpr auto streamThreads = 1000;
constexpr auto streamInput = 700;
constexpr auto streamOutput = 900;
constexpr auto streamGain = 0.75f;

Words streamTwin(const Vector<float>& input)
{
    auto output = makeFloats(streamOutput, sentinel);
    auto pairs = makeFloats(2 * streamOutput, sentinel);
    auto spread = makeFloats(3 * streamOutput, sentinel);

    for (auto i = 0u; i < (std::uint32_t) streamThreads; ++i)
    {
        auto sum = elementOr(input, i) * streamGain + elementOr(input, i + 1u);

        if (i < (std::uint32_t) output.size())
            output[(int) i] = sum;

        if (2 * i + 1 < (std::uint32_t) pairs.size())
        {
            pairs[(int) (2 * i)] = sum;
            pairs[(int) (2 * i + 1)] = elementOr(input, i * 3u);
        }

        if (3 * i + 1 < (std::uint32_t) spread.size())
            spread[(int) (3 * i + 1)] = elementOr(input, 2 * i + 1);
    }

    return joined(joined(bitsOf(output), bitsOf(pairs)), bitsOf(spread));
}

// ------------------------------------------------------ 2D and 3D, partial rows

struct PlaneKernel final : ComputeKernel
{
    explicit PlaneKernel(ThreadGroupShape shape)
        : ComputeKernel(shape)
    {
        compile();
    }

    void define() override
    {
        auto position = threadPosition();
        auto group = groupPosition();
        auto local = localPosition();
        auto cell = position.y * gridWidth() + position.x;
        auto tag = group.x * 1000000u + group.y * 10000u + local.x * 100u + local.y;
        write(output, cell, tag ^ (position.x * 7u + position.y));
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

Words planeTwin(ThreadGroupShape shape, int width, int height, int size)
{
    auto output = makeUInts(size, sentinelBits);

    for (auto y = 0u; y < (std::uint32_t) height; ++y)
    {
        for (auto x = 0u; x < (std::uint32_t) width; ++x)
        {
            auto groupX = x / (std::uint32_t) shape.x;
            auto groupY = y / (std::uint32_t) shape.y;
            auto localX = x % (std::uint32_t) shape.x;
            auto localY = y % (std::uint32_t) shape.y;
            auto tag = groupX * 1000000u + groupY * 10000u + localX * 100u + localY;
            output[(int) (y * (std::uint32_t) width + x)] = tag ^ (x * 7u + y);
        }
    }

    return output;
}

struct VolumeKernel final : ComputeKernel
{
    explicit VolumeKernel(ThreadGroupShape shape)
        : ComputeKernel(shape)
    {
        compile();
    }

    void define() override
    {
        auto position = threadPosition3();
        auto group = groupPosition3();
        auto local = localPosition3();
        auto cell =
            (position.z * gridHeight() + position.y) * gridWidth() + position.x;
        auto tag = (group.x * 31u + group.y) * 17u + group.z;
        write(output,
              cell,
              tag * 1000u + (local.x * 9u + local.y) * 5u + local.z + position.x);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

Words volumeTwin(ThreadGroupShape shape, int width, int height, int depth, int size)
{
    auto output = makeUInts(size, sentinelBits);
    auto sx = (std::uint32_t) shape.x;
    auto sy = (std::uint32_t) shape.y;
    auto sz = (std::uint32_t) shape.z;

    for (auto z = 0u; z < (std::uint32_t) depth; ++z)
    {
        for (auto y = 0u; y < (std::uint32_t) height; ++y)
        {
            for (auto x = 0u; x < (std::uint32_t) width; ++x)
            {
                auto tag = ((x / sx) * 31u + y / sy) * 17u + z / sz;
                auto local = ((x % sx) * 9u + y % sy) * 5u + z % sz;
                auto cell =
                    (z * (std::uint32_t) height + y) * (std::uint32_t) width + x;
                output[(int) cell] = tag * 1000u + local + x;
            }
        }
    }

    return output;
}

// --------------------------------------------------------- group and local ids

constexpr auto idGroup = 48;
constexpr auto idThreads = 1500;

struct IdArithmeticKernel final : ComputeKernel
{
    IdArithmeticKernel()
        : ComputeKernel({idGroup})
    {
        compile();
    }

    void define() override
    {
        auto group = groupId();
        auto local = localId();
        auto flat = group * (unsigned) idGroup + local;
        auto scaled = local * 1000u / (group + 1u) + local % (group % 5u + 1u);
        auto fromGroup = table[group] + toFloat(group * 3u);
        write(output, flat, uint2(scaled, flat - threadId()));
        write(values, threadId(), fromGroup * toFloat(local + 1u));
        write(lastGroup, local, toFloat(group) + table[local]);
    }

    Uniform<InputBuffer> table;
    Uniform<UIntOutputBuffer> output;
    Uniform<OutputBuffer> values;
    Uniform<OutputBuffer> lastGroup;

    EACP_SHADER(table, output, values, lastGroup)
};

constexpr auto idTable = 20;

Words idTwin(const Vector<float>& table)
{
    auto output = makeUInts(2 * idThreads + 8, sentinelBits);
    auto values = makeFloats(idThreads + 8, sentinel);
    auto lastGroup = makeFloats(idGroup + 8, sentinel);

    for (auto local = 0u; local < (std::uint32_t) idGroup; ++local)
    {
        auto final = ((std::uint32_t) idThreads - 1u - local) / (unsigned) idGroup;
        lastGroup[(int) local] = (float) final + elementOr(table, local);
    }

    for (auto i = 0u; i < (std::uint32_t) idThreads; ++i)
    {
        auto group = i / (unsigned) idGroup;
        auto local = i % (unsigned) idGroup;
        auto scaled = local * 1000u / (group + 1u) + local % (group % 5u + 1u);
        output[(int) (2 * i)] = scaled;
        output[(int) (2 * i + 1)] = 0u;

        auto fromGroup = elementOr(table, group) + (float) (group * 3u);
        values[(int) i] = fromGroup * (float) (local + 1u);
    }

    return joined(joined(output, bitsOf(values)), bitsOf(lastGroup));
}

// ------------------------------------------------------------ divergent flow

struct DivergentFlowKernel final : ComputeKernel
{
    DivergentFlowKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto n = var(0u);
        auto sum = var(0u);
        auto value = var(0.f);

        loop(n < i % 37u,
             [&]
             {
                 ifThen(n == 20u + (i & 3u), [&] { breakLoop(); });
                 n += 1u;
                 ifThen(n % 5u == 0u, [&] { continueLoop(); });
                 sum += n * (i & 7u);
             });

        ifThen(
            i % 3u == 0u,
            [&] { value = toFloat(sum.get()) * 0.5f; },
            [&] { value = toFloat(n.get()) + input[i]; });

        write(output, i, value.get());
        write(counts, i, sum.get() + n.get() * 1000u);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UIntOutputBuffer> counts;

    EACP_SHADER(input, output, counts)
};

constexpr auto flowThreads = 777;

Words flowTwin(const Vector<float>& input)
{
    auto output = makeFloats(flowThreads, sentinel);
    auto counts = makeUInts(flowThreads, sentinelBits);

    for (auto i = 0u; i < (std::uint32_t) flowThreads; ++i)
    {
        auto n = 0u;
        auto sum = 0u;

        while (n < i % 37u)
        {
            if (n == 20u + (i & 3u))
                break;

            n += 1u;

            if (n % 5u == 0u)
                continue;

            sum += n * (i & 7u);
        }

        output[(int) i] =
            i % 3u == 0u ? (float) sum * 0.5f : (float) n + elementOr(input, i);
        counts[(int) i] = sum + n * 1000u;
    }

    return joined(bitsOf(output), counts);
}

// ------------------------------------------------------ one statement, many hits

struct ConflictKernel final : ComputeKernel
{
    ConflictKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i / 100u, toFloat(i));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

// ------------------------------------------------------------------ indirect

struct CountRunsKernel final : ComputeKernel
{
    CountRunsKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, output[i] + toFloat(i % 9u) + 1.f);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

constexpr auto indirectSize = 1200;

Words indirectTwin(std::uint32_t groups, std::uint32_t rows, int guard)
{
    auto output = makeFloats(indirectSize, 0.f);
    auto threads = groups * (std::uint32_t) ComputePass::threadGroupWidth;

    for (auto row = 0u; row < rows; ++row)
        for (auto i = 0u; i < threads && i < (std::uint32_t) guard; ++i)
            if (i < (std::uint32_t) output.size())
                output[(int) i] = output[(int) i] + (float) (i % 9u) + 1.f;

    return bitsOf(output);
}

// ------------------------------------------------------- group-scope features

struct BarrierKernel final : ComputeKernel
{
    BarrierKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        barrier();
        write(output, i, constant(1.f));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct SharedKernel final : ComputeKernel
{
    SharedKernel() { compile(); }

    void define() override
    {
        auto tile = shared<Float>(4);
        write(tile, localId(), constant(2.f));
        write(output, threadId(), tile[0u]);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct ReductionKernel final : ComputeKernel
{
    ReductionKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, groupSum(toFloat(i)));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct SimdReductionKernel final : ComputeKernel
{
    SimdReductionKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, simdSum(toFloat(i)));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct SimdIndexKernel final : ComputeKernel
{
    SimdIndexKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, simdGroupIndex());
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

struct AtomicAddKernel final : ComputeKernel
{
    AtomicAddKernel() { compile(); }

    void define() override { atomicAdd(counter, 0u, 1u); }

    Uniform<AtomicBuffer> counter;

    EACP_SHADER(counter)
};

struct AtomicLoadKernel final : ComputeKernel
{
    AtomicLoadKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, counter.load(0u));
    }

    Uniform<AtomicBuffer> counter;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(counter, output)
};

template <typename Kernel>
bool runsOneGroupPerBatch()
{
    auto kernel = Kernel {};
    auto executor = Executor {kernel, PlanOptions {1 << 16, unlimitedBytes}};
    check(executor.isValid(), executor.reason());
    return executor.plan().groupsPerBatch() == 1
           && executor.plan().batchLanes() == executor.plan().lanes();
}
} // namespace

auto tBatchStream =
    test("Batch/aStreamWithRampsAndAnOutOfRangeTailMatchesAtEveryWidth") = []
{
    auto input = wobble(streamInput);
    auto kernel = StreamKernel {};
    kernel.gain = streamGain;

    checkEveryWidth(kernel,
                    streamTwin(input),
                    [&](Executor& executor)
                    {
                        auto output = makeFloats(streamOutput, sentinel);
                        auto pairs = makeFloats(2 * streamOutput, sentinel);
                        auto spread = makeFloats(3 * streamOutput, sentinel);

                        auto bindings = Bindings {};
                        bindings.set(kernel.input, input);
                        bindings.set(kernel.output, output);
                        bindings.set(kernel.pairs, pairs);
                        bindings.set(kernel.spread, spread);
                        check(executor.dispatch(bindings, streamThreads));

                        return joined(joined(bitsOf(output), bitsOf(pairs)),
                                      bitsOf(spread));
                    });
};

auto tBatchStreamRamps = test("Batch/aWideStreamStillTakesTheRampPaths") = []
{
    auto kernel = StreamKernel {};
    auto executor = Executor {kernel, batchesOf(kernel, 16)};
    check(executor.isValid(), executor.reason());

    const auto& plan = executor.plan();
    auto rampStores = 0;

    for (auto id = 0; id < plan.stepCount(); ++id)
        rampStores += plan.step(id).ramp ? 1 : 0;

    check(rampStores == 3);
};

auto tBatchPlane = test("Batch/twoDimensionalDispatchesWithPartialRowsMatch") = []
{
    struct Case
    {
        ThreadGroupShape shape;
        int width;
        int height;
    };

    for (auto item:
         {Case {{16, 4}, 37, 9}, Case {{8, 1}, 100, 5}, Case {{3, 2}, 50, 7}})
    {
        auto kernel = PlaneKernel {item.shape};
        auto size = item.width * item.height + 40;

        checkEveryWidth(
            kernel,
            planeTwin(item.shape, item.width, item.height, size),
            [&](Executor& executor)
            {
                auto output = makeUInts(size, sentinelBits);
                auto bindings = Bindings {};
                bindings.set(kernel.output, output);
                check(executor.dispatch(bindings, item.width, item.height));
                return output;
            });
    }
};

auto tBatchVolume = test("Batch/threeDimensionalDispatchesWithPartialRowsMatch") = []
{
    struct Case
    {
        ThreadGroupShape shape;
        int width;
        int height;
        int depth;
    };

    for (auto item: {Case {{2, 3, 4}, 23, 7, 9}, Case {{4, 1, 1}, 45, 3, 2}})
    {
        auto kernel = VolumeKernel {item.shape};
        auto size = item.width * item.height * item.depth + 40;

        checkEveryWidth(
            kernel,
            volumeTwin(item.shape, item.width, item.height, item.depth, size),
            [&](Executor& executor)
            {
                auto output = makeUInts(size, sentinelBits);
                auto bindings = Bindings {};
                bindings.set(kernel.output, output);
                check(executor.dispatch(
                    bindings, item.width, item.height, item.depth));
                return output;
            });
    }
};

auto tBatchIds = test("Batch/groupAndLocalIdArithmeticMatchesAtEveryWidth") = []
{
    auto table = wobble(idTable);
    auto kernel = IdArithmeticKernel {};

    checkEveryWidth(kernel,
                    idTwin(table),
                    [&](Executor& executor)
                    {
                        auto output = makeUInts(2 * idThreads + 8, sentinelBits);
                        auto values = makeFloats(idThreads + 8, sentinel);
                        auto lastGroup = makeFloats(idGroup + 8, sentinel);

                        auto bindings = Bindings {};
                        bindings.set(kernel.table, table);
                        bindings.set(kernel.output, output);
                        bindings.set(kernel.values, values);
                        bindings.set(kernel.lastGroup, lastGroup);
                        check(executor.dispatch(bindings, idThreads));

                        return joined(joined(output, bitsOf(values)),
                                      bitsOf(lastGroup));
                    });
};

auto tBatchFlow = test("Batch/divergentBranchesAndLoopsMatchAtEveryWidth") = []
{
    auto input = wobble(flowThreads);
    auto kernel = DivergentFlowKernel {};

    checkEveryWidth(kernel,
                    flowTwin(input),
                    [&](Executor& executor)
                    {
                        auto output = makeFloats(flowThreads, sentinel);
                        auto counts = makeUInts(flowThreads, sentinelBits);

                        auto bindings = Bindings {};
                        bindings.set(kernel.input, input);
                        bindings.set(kernel.output, output);
                        bindings.set(kernel.counts, counts);
                        check(executor.dispatch(bindings, flowThreads));

                        return joined(bitsOf(output), counts);
                    });
};

auto tBatchConflict = test("Batch/aStoreManyThreadsShareKeepsTheLastThread") = []
{
    constexpr auto threads = 1000;
    auto kernel = ConflictKernel {};
    auto twin = makeFloats(12, sentinel);

    // The last thread of each hundred, in closed form: the loop that stores
    // every thread in turn is vectorised out of order by Apple clang 21 -O3.
    for (auto element = 0; element < threads / 100; ++element)
        twin[element] = (float) (element * 100 + 99);

    checkEveryWidth(kernel,
                    bitsOf(twin),
                    [&](Executor& executor)
                    {
                        auto output = makeFloats(12, sentinel);
                        auto bindings = Bindings {};
                        bindings.set(kernel.output, output);
                        check(executor.dispatch(bindings, threads));
                        return bitsOf(output);
                    });
};

auto tBatchIndirect =
    test("Batch/anIndirectDispatchMasksTheGroupsItDidNotAskFor") = []
{
    struct Case
    {
        std::uint32_t groups;
        std::uint32_t rows;
        int guard;
    };

    // Five groups of a sixteen-group batch under a guard that passes all of
    // them, so only the batch's own mask keeps the other eleven out; then a
    // guard inside the groups, and rows that repeat the x range.
    for (auto item: {Case {5, 1, 1100}, Case {7, 1, 300}, Case {3, 3, 1100}})
    {
        auto kernel = CountRunsKernel {};
        auto arguments = Words {item.groups, item.rows, 1u};

        checkEveryWidth(
            kernel,
            indirectTwin(item.groups, item.rows, item.guard),
            [&](Executor& executor)
            {
                auto output = makeFloats(indirectSize, 0.f);
                auto bindings = Bindings {};
                bindings.set(kernel.output, output);
                check(executor.dispatchIndirect(bindings, arguments, item.guard));
                return bitsOf(output);
            });
    }
};

auto tBatchGroupScope = test("Batch/aKernelWithAGroupScopeFeatureKeepsOneGroup") = []
{
    check(runsOneGroupPerBatch<BarrierKernel>());
    check(runsOneGroupPerBatch<SharedKernel>());
    check(runsOneGroupPerBatch<ReductionKernel>());
    check(runsOneGroupPerBatch<SimdReductionKernel>());
    check(runsOneGroupPerBatch<SimdIndexKernel>());
    check(runsOneGroupPerBatch<AtomicAddKernel>());
    check(runsOneGroupPerBatch<AtomicLoadKernel>());

    auto stream = StreamKernel {};
    check(Executor {stream, PlanOptions {1 << 16, unlimitedBytes}}
              .plan()
              .groupsPerBatch()
          == (1 << 16) / ComputePass::threadGroupWidth);
    check(Executor {stream, PlanOptions {1}}.plan().groupsPerBatch() == 1);
};

auto tBatchDefaultWidth = test("Batch/theDefaultBatchAndItsFootprint") = []
{
    auto kernel = StreamKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    const auto& plan = executor.plan();
    check(plan.batchLanes() == PlanOptions::defaultBatchLanes);
    check(plan.groupsPerBatch()
          == PlanOptions::defaultBatchLanes / ComputePass::threadGroupWidth);
    check(plan.laneStride() == plan.batchLanes());

    auto narrow = Executor {kernel, batchesOf(kernel, 1)};
    std::printf("    StreamKernel footprint: %zu bytes at %d lanes, %zu at %d\n",
                plan.footprintBytes(),
                plan.batchLanes(),
                narrow.plan().footprintBytes(),
                narrow.plan().batchLanes());

    check(plan.footprintBytes() <= PlanOptions::defaultBatchBytes);
};

// -------------------------------------------------------------- batch budget

namespace
{
constexpr auto tableThreads = 3000;

// out[i] = table[i % Elements].x + .w over a table of Elements float4
// literals, whose storage is a row per element per lane of the batch.
template <int Elements>
struct TableGraph
{
    TableGraph()
    {
        auto i = builder.threadId();
        auto table = makeTable(std::make_index_sequence<Elements> {});
        auto entry = table[toInt(i % (unsigned) Elements)];
        builder.write(output, i, entry.x() + entry.w());
    }

    template <std::size_t... k>
    ConstantArray<Float4, Elements> makeTable(std::index_sequence<k...>)
    {
        return builder.array(
            float4(builder.constant((float) k), 0.5f, 0.25f, (float) (k * 3))...);
    }

    Words run(Executor& executor)
    {
        auto values = makeFloats(tableThreads, sentinel);
        auto bindings = Bindings {};
        bindings.set(output, values);
        check(executor.dispatch(bindings, tableThreads));
        return bitsOf(values);
    }

    static Words twin()
    {
        auto values = Vector<float> {};

        for (auto k = 0; k < tableThreads; ++k)
            values.add((float) (k % Elements) + (float) (k % Elements * 3));

        return bitsOf(values);
    }

    ShaderBuilder builder;
    OutputBuffer output = builder.outputBuffer();
};

PlanOptions lanesWithoutBudget(int lanes)
{
    return {lanes, unlimitedBytes};
}
} // namespace

auto tBatchBigTable =
    test("Batch/aLargeArrayConstantBacksOffToOneGroupPerBatch") = []
{
    auto graph = TableGraph<256> {};
    auto executor = Executor {graph.builder.graph()};
    check(executor.isValid(), executor.reason());

    const auto& plan = executor.plan();
    check(plan.groupsPerBatch() == 1);

    auto oneGroup = Executor {graph.builder.graph(), PlanOptions {1}};
    auto unbounded = Executor {graph.builder.graph(),
                               lanesWithoutBudget(PlanOptions::defaultBatchLanes)};
    check(unbounded.plan().groupsPerBatch() > 1);
    check(plan.footprintBytes() == oneGroup.plan().footprintBytes());
    check(plan.footprintBytes() < unbounded.plan().footprintBytes());

    std::printf("    256-entry float4 table: %zu bytes at %d lanes, %zu at %d\n",
                plan.footprintBytes(),
                plan.batchLanes(),
                unbounded.plan().footprintBytes(),
                unbounded.plan().batchLanes());

    auto words = graph.run(executor);
    check(sameWords(words, graph.twin(), 1));
    check(sameWords(graph.run(unbounded), words, unbounded.plan().groupsPerBatch()));
};

auto tBatchHalved = test("Batch/aBatchOverTheBudgetIsHalvedUntilItFits") = []
{
    auto graph = TableGraph<16> {};
    auto executor = Executor {graph.builder.graph()};
    check(executor.isValid(), executor.reason());

    const auto& plan = executor.plan();
    auto groups = plan.groupsPerBatch();
    auto lanes = plan.lanes();
    check(groups > 1);
    check(groups < PlanOptions::defaultBatchLanes / lanes);
    check(plan.footprintBytes() <= PlanOptions::defaultBatchBytes);

    auto doubled =
        Executor {graph.builder.graph(), lanesWithoutBudget(2 * groups * lanes)};
    check(doubled.plan().groupsPerBatch() == 2 * groups);
    check(doubled.plan().footprintBytes() > PlanOptions::defaultBatchBytes);

    auto tight = Executor {
        graph.builder.graph(),
        PlanOptions {PlanOptions::defaultBatchLanes, plan.footprintBytes() - 1}};
    check(tight.plan().groupsPerBatch() == groups / 2);

    std::printf("    16-entry float4 table: %d groups per batch, %zu bytes\n",
                groups,
                plan.footprintBytes());

    check(sameWords(graph.run(executor), graph.twin(), groups));
};
