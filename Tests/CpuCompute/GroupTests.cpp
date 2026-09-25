#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

// The threadgroup tier: local and group ids, shared memory, barriers,
// reductions, atomics, the SIMD-group index and the indirect dispatch. Every
// case carries its C++ twin; the reductions' twin folds by the same halving
// tree the D3D12 and Vulkan fallback emits, so float sums compare exactly.

namespace
{
constexpr auto sentinel = -1.f;
constexpr auto sentinelBits = 0xdeadbeefu;

Vector<float> makeFloats(int count, float value)
{
    auto values = Vector<float> {};
    values.resize(count, value);
    return values;
}

Vector<std::uint32_t> makeUInts(int count, std::uint32_t value)
{
    auto values = Vector<std::uint32_t> {};
    values.resize(count, value);
    return values;
}

template <typename T>
T elementOr(const Vector<T>& values, int index, T fallback)
{
    return index >= 0 && index < values.size() ? values[index] : fallback;
}

// The fallback's tree over lanes [first, end) of a block `width` wide: at each
// halving step, lane w of the block takes fold(itself, lane w + step).
template <typename T, typename Fold>
T halvingFold(Vector<T> lanes, int first, int width, Fold fold)
{
    auto end = lanes.size();
    auto step = 1;

    while (step * 2 < width)
        step *= 2;

    for (; step > 0; step >>= 1)
        for (auto within = 0;
             within < step && within + step < width && first + within + step < end;
             ++within)
            lanes[first + within] =
                fold(lanes[first + within], lanes[first + within + step]);

    return lanes[first];
}

float addFloats(float a, float b)
{
    return a + b;
}

float maxFloats(float a, float b)
{
    return std::fmax(a, b);
}

float minFloats(float a, float b)
{
    return std::fmin(a, b);
}

std::uint32_t addUInts(std::uint32_t a, std::uint32_t b)
{
    return a + b;
}

std::uint32_t maxUInts(std::uint32_t a, std::uint32_t b)
{
    return std::max(a, b);
}

std::uint32_t minUInts(std::uint32_t a, std::uint32_t b)
{
    return std::min(a, b);
}

template <typename T>
Vector<T> groupLanes(const Vector<T>& values, int group, int width, T fallback)
{
    auto lanes = Vector<T> {};

    for (auto lane = 0; lane < width; ++lane)
        lanes.add(elementOr(values, group * width + lane, fallback));

    return lanes;
}
} // namespace

// ---------------------------------------------------------------------------
// Where a thread sits: local and group ids over custom shapes of every rank.

namespace
{
constexpr auto wideGroup = 48;

struct FlatIdsKernel final : ComputeKernel
{
    FlatIdsKernel()
        : ComputeKernel({wideGroup})
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        write(output, i, uint2(localId(), groupId()));
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

constexpr auto tileWidth = 5;
constexpr auto tileHeight = 3;

struct TileIdsKernel final : ComputeKernel
{
    TileIdsKernel()
        : ComputeKernel({tileWidth, tileHeight})
    {
        compile();
    }

    void define() override
    {
        auto position = threadPosition();
        auto local = localPosition();
        auto group = groupPosition();
        auto cell = position.y * gridWidth() + position.x;
        write(output, cell, uint4(local.x, local.y, group.x, group.y));
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

auto tFlatIds = test("Group/localAndGroupIdsFollowAOneDimensionalShape") = []
{
    constexpr auto count = 100;

    auto kernel = FlatIdsKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeUInts(3 * wideGroup * 2, sentinelBits);
    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
    {
        check(output[i * 2] == (std::uint32_t) (i % wideGroup));
        check(output[i * 2 + 1] == (std::uint32_t) (i / wideGroup));
    }

    for (auto k = count * 2; k < output.size(); ++k)
        check(output[k] == sentinelBits);
};

auto tTileIds = test("Group/localAndGroupPositionsFollowATwoDimensionalShape") = []
{
    constexpr auto width = 12;
    constexpr auto height = 7;

    auto kernel = TileIdsKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeUInts(width * height * 4 + 8, sentinelBits);
    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, width, height));

    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            auto at = (y * width + x) * 4;
            check(output[at + 0] == (std::uint32_t) (x % tileWidth));
            check(output[at + 1] == (std::uint32_t) (y % tileHeight));
            check(output[at + 2] == (std::uint32_t) (x / tileWidth));
            check(output[at + 3] == (std::uint32_t) (y / tileHeight));
        }
    }

    for (auto k = width * height * 4; k < output.size(); ++k)
        check(output[k] == sentinelBits);
};

auto tVolumeIds = test("Group/vectorLocalAndGroupIdsInThreeDimensions") = []
{
    constexpr auto width = 5;
    constexpr auto height = 7;
    constexpr auto depth = 9;
    constexpr auto shape = std::array {2, 3, 4};

    auto builder = ShaderBuilder {};
    builder.setThreadGroupShape({shape[0], shape[1], shape[2]});

    auto output = builder.uintOutputBuffer();
    auto position = builder.threadPosition3();
    auto cell =
        ((position.z * builder.gridHeight() + position.y) * builder.gridWidth()
         + position.x)
        * 2u;
    builder.write(output, cell, builder.localId3());
    builder.write(output, cell + 1u, builder.groupId3());

    auto executor = Executor {builder.graph()};
    check(executor.isValid(), executor.reason());

    auto values = makeUInts(width * height * depth * 6, sentinelBits);
    auto bindings = Bindings {};
    bindings.set(output, values);
    check(executor.dispatch(bindings, width, height, depth));

    for (auto z = 0; z < depth; ++z)
    {
        for (auto y = 0; y < height; ++y)
        {
            for (auto x = 0; x < width; ++x)
            {
                auto at = ((z * height + y) * width + x) * 6;
                auto coordinates = std::array {x, y, z};

                for (auto axis = 0; axis < 3; ++axis)
                {
                    auto extent = shape[(std::size_t) axis];
                    auto value = coordinates[(std::size_t) axis];
                    check(values[at + axis] == (std::uint32_t) (value % extent));
                    check(values[at + 3 + axis] == (std::uint32_t) (value / extent));
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Shared memory: one array per group, zero at its start, bounded per D7.

namespace
{
constexpr auto groupWidth = ComputePass::threadGroupWidth;

struct ReverseKernel final : ComputeKernel
{
    ReverseKernel() { compile(); }

    void define() override
    {
        auto lane = localId();
        auto i = threadId();
        auto tile = shared<Float>(groupWidth);

        write(tile, lane, input[i]);
        barrier();

        ifThen(i < gridCount(),
               [&] { write(output, i, tile[(unsigned) groupWidth - 1u - lane]); });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

constexpr auto vectorTile = 32;

struct VectorTileKernel final : ComputeKernel
{
    VectorTileKernel()
        : ComputeKernel({vectorTile})
    {
        compile();
    }

    void define() override
    {
        auto lane = localId();
        auto i = threadId();
        auto tile = shared<Float3>(vectorTile);
        auto x = input[i];

        write(tile, lane, float3(x, x * 2.f, x * 3.f));
        barrier();

        ifThen(i < gridCount(),
               [&] { write(output, i, tile[(lane + 1u) % (unsigned) vectorTile]); });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct AlternateGroupsKernel final : ComputeKernel
{
    AlternateGroupsKernel() { compile(); }

    void define() override
    {
        auto lane = localId();
        auto i = threadId();
        auto tile = shared<Float>(groupWidth);

        ifThen(groupId() % 2u == 0u,
               [&] { write(tile, lane, toFloat(lane) + 1.f); });
        barrier();

        ifThen(i < gridCount(), [&] { write(output, i, tile[lane]); });
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct SharedBoundsKernel final : ComputeKernel
{
    SharedBoundsKernel() { compile(); }

    void define() override
    {
        auto lane = localId();
        auto i = threadId();
        auto first = shared<Float>(groupWidth);
        auto second = shared<Float>(groupWidth);

        write(first, lane + (unsigned) groupWidth, constant(9.f));
        write(second, lane, constant(1.f));
        barrier();

        ifThen(i < gridCount(),
               [&]
               {
                   write(output,
                         i,
                         float2(first[lane + (unsigned) groupWidth], second[lane]));
               });
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

auto tSharedAcrossBarrier =
    test("Group/sharedMemoryCarriesValuesAcrossABarrier") = []
{
    constexpr auto count = 150;

    auto kernel = ReverseKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());
    check(!executor.plan().guardsBounds());

    auto input = makeFloats(count, 0.f);

    for (auto k = 0; k < count; ++k)
        input[k] = (float) (k * 3 + 1);

    auto output = makeFloats(count + 10, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
    {
        auto group = i / groupWidth;
        auto mirrored = group * groupWidth + groupWidth - 1 - i % groupWidth;
        check(output[i] == elementOr(input, mirrored, 0.f));
    }

    for (auto k = count; k < output.size(); ++k)
        check(output[k] == sentinel);
};

auto tVectorShared = test("Group/aVectorSharedArrayHoldsWholeElements") = []
{
    constexpr auto count = 70;

    auto kernel = VectorTileKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto input = makeFloats(count, 0.f);

    for (auto k = 0; k < count; ++k)
        input[k] = (float) k + 0.5f;

    auto output = makeFloats(count * 3, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
    {
        auto group = i / vectorTile;
        auto next = group * vectorTile + (i % vectorTile + 1) % vectorTile;
        auto x = elementOr(input, next, 0.f);
        check(output[i * 3 + 0] == x);
        check(output[i * 3 + 1] == x * 2.f);
        check(output[i * 3 + 2] == x * 3.f);
    }
};

auto tSharedZeroed = test("Group/sharedMemoryStartsEveryGroupZeroed") = []
{
    constexpr auto count = groupWidth * 4;

    auto kernel = AlternateGroupsKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeFloats(count, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.output, output);

    for (auto round = 0; round < 2; ++round)
    {
        check(executor.dispatch(bindings, count));

        for (auto i = 0; i < count; ++i)
        {
            auto evenGroup = (i / groupWidth) % 2 == 0;
            check(output[i] == (evenGroup ? (float) (i % groupWidth) + 1.f : 0.f));
        }
    }
};

auto tSharedBounds =
    test("Group/aSharedIndexOutOfBoundsReadsZeroAndDropsTheStore") = []
{
    constexpr auto count = groupWidth * 2;

    auto kernel = SharedBoundsKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeFloats(count * 2, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
    {
        check(output[i * 2] == 0.f);
        check(output[i * 2 + 1] == 1.f);
    }
};

// ---------------------------------------------------------------------------
// Barriers: a no-op in lockstep, inside a loop and under divergence alike.

namespace
{
struct ScanKernel final : ComputeKernel
{
    ScanKernel() { compile(); }

    void define() override
    {
        auto lane = localId();
        auto i = threadId();
        auto tile = shared<Float>(groupWidth);
        auto step = var(1u);

        write(tile, lane, input[i]);
        barrier();

        loop(step.get() < (unsigned) groupWidth,
             [&]
             {
                 auto addend = var(0.f);

                 ifThen(lane >= step.get(),
                        [&] { addend = tile[lane - step.get()]; });

                 barrier();
                 write(tile, lane, tile[lane] + addend.get());
                 barrier();
                 step = step.get() * 2u;
             });

        ifThen(i < gridCount(), [&] { write(output, i, tile[lane]); });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct DivergentBarrierKernel final : ComputeKernel
{
    DivergentBarrierKernel() { compile(); }

    void define() override
    {
        auto lane = localId();
        auto i = threadId();
        auto tile = shared<Float>(groupWidth);
        auto turns = var(0u);

        ifThen(
            lane % 2u == 0u,
            [&]
            {
                write(tile, lane, toFloat(lane));
                barrier();
            },
            [&]
            {
                barrier();
                write(tile, lane, -toFloat(lane));
            });

        loop(turns.get() < lane % 4u,
             [&]
             {
                 barrier();
                 turns = turns.get() + 1u;
             });

        barrier();

        ifThen(i < gridCount(),
               [&]
               { write(output, i, float2(tile[lane ^ 1u], toFloat(turns.get()))); });
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

auto tBarrierLoop = test("Group/aBarrierInsideALoopOrdersEachRound") = []
{
    constexpr auto count = 150;

    auto kernel = ScanKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto input = makeFloats(count, 0.f);

    for (auto k = 0; k < count; ++k)
        input[k] = (float) (k % 11 + 1);

    auto output = makeFloats(count, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    auto running = 0.f;

    for (auto i = 0; i < count; ++i)
    {
        if (i % groupWidth == 0)
            running = 0.f;

        running += input[i];
        check(output[i] == running);
    }
};

auto tBarrierDivergence = test("Group/aBarrierUnderDivergenceIsANoOp") = []
{
    constexpr auto count = groupWidth * 2;

    auto kernel = DivergentBarrierKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeFloats(count * 2, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
    {
        auto lane = i % groupWidth;
        auto partner = lane ^ 1;
        auto expected = partner % 2 == 0 ? (float) partner : -(float) partner;
        check(output[i * 2] == expected);
        check(output[i * 2 + 1] == (float) (lane % 4));
    }
};

// ---------------------------------------------------------------------------
// Reductions: the fallback's halving tree, both scopes, and D7's divergence.

namespace
{
constexpr auto oddGroup = 48;

struct FoldKernel final : ComputeKernel
{
    FoldKernel()
        : ComputeKernel({oddGroup})
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        auto value = input[i];
        auto word = words[i];

        write(floats,
              i,
              float4(groupSum(value), groupMax(value), groupMin(value), 0.f));
        write(uints, i, uint4(groupSum(word), groupMax(word), groupMin(word), 0u));
    }

    Uniform<InputBuffer> input;
    Uniform<UIntInputBuffer> words;
    Uniform<OutputBuffer> floats;
    Uniform<UIntOutputBuffer> uints;

    EACP_SHADER(input, words, floats, uints)
};

constexpr auto threeSimdGroups = 3 * simdGroupWidth;

struct SimdFoldKernel final : ComputeKernel
{
    SimdFoldKernel()
        : ComputeKernel({threeSimdGroups})
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        auto value = input[i];

        write(output,
              i,
              float4(simdSum(value),
                     simdMax(value),
                     simdMin(value),
                     toFloat(simdSum(words[i]))));
    }

    Uniform<InputBuffer> input;
    Uniform<UIntInputBuffer> words;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, words, output)
};

struct DivergentFoldKernel final : ComputeKernel
{
    DivergentFoldKernel() { compile(); }

    void define() override
    {
        auto lane = localId();
        auto i = threadId();
        auto value = input[i];

        ifThen(
            lane % 3u == 1u,
            [&]
            {
                write(floats, i, float2(groupSum(value), groupMax(value)));
                write(uints, i, groupMin(words[i]));
            },
            [&]
            {
                auto marker = constant(sentinel);
                write(floats, i, float2(marker, marker));
            });
    }

    Uniform<InputBuffer> input;
    Uniform<UIntInputBuffer> words;
    Uniform<OutputBuffer> floats;
    Uniform<UIntOutputBuffer> uints;

    EACP_SHADER(input, words, floats, uints)
};

constexpr auto rowCount = 5;

struct RowSumKernel final : ComputeKernel
{
    RowSumKernel() { compile(); }

    void define() override
    {
        auto lane = localId();
        auto row = var(0u);

        loop(row.get() < rows,
             [&]
             {
                 auto total =
                     groupSum(input[row.get() * (unsigned) groupWidth + lane]);

                 ifThen(lane == 0u, [&] { write(output, row.get(), total); });
                 row = row.get() + 1u;
             });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UInt> rows;

    EACP_SHADER(input, output, rows)
};

float sequentialSum(const Vector<float>& lanes)
{
    auto sum = 0.f;

    for (auto value: lanes)
        sum += value;

    return sum;
}

Vector<float> unevenFloats(int count)
{
    auto values = Vector<float> {};

    for (auto k = 0; k < count; ++k)
        values.add(k % 5 == 0 ? 16777216.f + (float) (k * 2) : 0.75f + (float) k);

    return values;
}
} // namespace

auto tGroupFolds = test("Group/groupReductionsFoldByTheHalvingTree") = []
{
    constexpr auto count = 100;
    constexpr auto groups = (count + oddGroup - 1) / oddGroup;
    constexpr auto lanes = groups * oddGroup;

    auto kernel = FoldKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto input = unevenFloats(count);
    auto words = makeUInts(count, 0u);

    for (auto k = 0; k < count; ++k)
    {
        input[k] = k % 2 == 0 ? input[k] : -input[k];
        words[k] = (std::uint32_t) ((k * 2654435761u) >> 7);
    }

    auto floats = makeFloats(lanes * 4, sentinel);
    auto uints = makeUInts(lanes * 4, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.words, words);
    bindings.set(kernel.floats, floats);
    bindings.set(kernel.uints, uints);
    check(executor.dispatch(bindings, count));

    auto orderMatters = false;

    for (auto group = 0; group < groups; ++group)
    {
        auto values = groupLanes(input, group, oddGroup, 0.f);
        auto bits = groupLanes(words, group, oddGroup, 0u);

        auto sum = halvingFold(values, 0, oddGroup, addFloats);
        orderMatters = orderMatters || sum != sequentialSum(values);

        for (auto lane = 0; lane < oddGroup; ++lane)
        {
            auto at = (group * oddGroup + lane) * 4;
            check(floats[at + 0] == sum);
            check(floats[at + 1] == halvingFold(values, 0, oddGroup, maxFloats));
            check(floats[at + 2] == halvingFold(values, 0, oddGroup, minFloats));
            check(uints[at + 0] == halvingFold(bits, 0, oddGroup, addUInts));
            check(uints[at + 1] == halvingFold(bits, 0, oddGroup, maxUInts));
            check(uints[at + 2] == halvingFold(bits, 0, oddGroup, minUInts));
        }
    }

    check(orderMatters);
};

auto tSimdFolds = test("Group/simdReductionsFoldEachBlockOfThirtyTwo") = []
{
    constexpr auto count = 2 * threeSimdGroups;

    auto kernel = SimdFoldKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto input = unevenFloats(count);
    auto words = makeUInts(count, 0u);

    for (auto k = 0; k < count; ++k)
        words[k] = (std::uint32_t) (k * k);

    auto output = makeFloats(count * 4, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.words, words);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
    {
        auto block = i / simdGroupWidth;
        auto values = groupLanes(input, block, simdGroupWidth, 0.f);
        auto bits = groupLanes(words, block, simdGroupWidth, 0u);

        check(output[i * 4 + 0]
              == halvingFold(values, 0, simdGroupWidth, addFloats));
        check(output[i * 4 + 1]
              == halvingFold(values, 0, simdGroupWidth, maxFloats));
        check(output[i * 4 + 2]
              == halvingFold(values, 0, simdGroupWidth, minFloats));
        check(output[i * 4 + 3]
              == (float) halvingFold(bits, 0, simdGroupWidth, addUInts));
    }
};

auto tPartialSimdBlock =
    test("Group/aSimdFoldInAGroupOfNoWholeBlocksFoldsWhatIsThere") = []
{
    for (auto width: {40, 16})
    {
        auto builder = ShaderBuilder {};
        builder.setThreadGroupShape({width});

        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto i = builder.threadId();
        builder.write(output, i, builder.simdSum(input[i]));

        auto executor = Executor {builder.graph()};
        check(executor.isValid(), executor.reason());

        auto values = unevenFloats(width);
        auto results = makeFloats(width, sentinel);
        auto bindings = Bindings {};
        bindings.set(input, values);
        bindings.set(output, results);
        check(executor.dispatch(bindings, width));

        auto blockWidth = std::min(width, simdGroupWidth);

        for (auto lane = 0; lane < width; ++lane)
        {
            auto first = lane / blockWidth * blockWidth;
            check(results[lane]
                  == halvingFold(values, first, blockWidth, addFloats));
        }
    }
};

auto tDivergentFold =
    test("Group/aReductionUnderDivergenceFoldsOnlyActiveLanes") = []
{
    constexpr auto count = groupWidth * 2;

    auto kernel = DivergentFoldKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto input = makeFloats(count, 0.f);
    auto words = makeUInts(count, 0u);

    for (auto k = 0; k < count; ++k)
    {
        input[k] = -0.5f * (float) (k + 1);
        words[k] = 1000u + (std::uint32_t) k;
    }

    auto floats = makeFloats(count * 2, 0.f);
    auto uints = makeUInts(count, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.words, words);
    bindings.set(kernel.floats, floats);
    bindings.set(kernel.uints, uints);
    check(executor.dispatch(bindings, count));

    for (auto group = 0; group < count / groupWidth; ++group)
    {
        auto values = Vector<float> {};
        auto bits = Vector<std::uint32_t> {};

        for (auto lane = 0; lane < groupWidth; ++lane)
        {
            auto active = lane % 3 == 1;
            auto k = group * groupWidth + lane;
            values.add(active ? input[k] : -std::numeric_limits<float>::infinity());
            bits.add(active ? words[k] : UINT32_MAX);
        }

        auto sums = values;

        for (auto lane = 0; lane < groupWidth; ++lane)
            if (lane % 3 != 1)
                sums[lane] = -0.f;

        auto sum = halvingFold(sums, 0, groupWidth, addFloats);
        auto peak = halvingFold(values, 0, groupWidth, maxFloats);
        auto least = halvingFold(bits, 0, groupWidth, minUInts);

        check(peak == input[group * groupWidth + 1]);
        check(least == words[group * groupWidth + 1]);

        for (auto lane = 0; lane < groupWidth; ++lane)
        {
            auto i = group * groupWidth + lane;

            if (lane % 3 == 1)
            {
                check(floats[i * 2] == sum);
                check(floats[i * 2 + 1] == peak);
                check(uints[i] == least);
            }
            else
            {
                check(floats[i * 2] == sentinel);
                check(floats[i * 2 + 1] == sentinel);
                check(uints[i] == sentinelBits);
            }
        }
    }
};

auto tFoldInLoop = test("Group/aReductionInsideALoopFoldsEachRound") = []
{
    auto kernel = RowSumKernel {};
    kernel.rows = (unsigned) rowCount;

    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto input = unevenFloats(rowCount * groupWidth);
    auto output = makeFloats(rowCount + 1, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, groupWidth));

    for (auto row = 0; row < rowCount; ++row)
        check(
            output[row]
            == halvingFold(
                groupLanes(input, row, groupWidth, 0.f), 0, groupWidth, addFloats));

    check(output[rowCount] == sentinel);
};

// ---------------------------------------------------------------------------
// Atomics: relaxed adds through std::atomic_ref, lanes in ascending order.

namespace
{
struct TicketKernel final : ComputeKernel
{
    TicketKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto ticket = atomicAdd(counter, 0u, 1u);
        write(output, ticket, i);
    }

    Uniform<AtomicBuffer> counter;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(counter, output)
};

constexpr auto bucketCount = 4u;

struct BucketKernel final : ComputeKernel
{
    BucketKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto before = atomicAdd(counts, i % bucketCount, 1u);
        auto outside = atomicAdd(counts, i + bucketCount, 5u);
        write(output, i, uint2(before, outside));
    }

    Uniform<AtomicBuffer> counts;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(counts, output)
};

struct LoadKernel final : ComputeKernel
{
    LoadKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, toFloat(counts.load(i)));
    }

    Uniform<AtomicBuffer> counts;
    Uniform<OutputBuffer> output;

    EACP_SHADER(counts, output)
};
} // namespace

auto tTickets = test("Group/atomicAddsHandEveryLaneADistinctSlot") = []
{
    constexpr auto count = 150;

    auto kernel = TicketKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto counter = makeUInts(1, 0u);
    auto output = makeUInts(count + 8, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.counter, counter);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    check(counter[0] == (std::uint32_t) count);

    auto seen = makeUInts(count, 0u);

    for (auto ticket = 0; ticket < count; ++ticket)
    {
        auto thread = output[ticket];
        check(thread < (std::uint32_t) count);

        if (thread < (std::uint32_t) count)
            seen[(int) thread] += 1u;

        check(thread == (std::uint32_t) ticket);
    }

    for (auto k = 0; k < count; ++k)
        check(seen[k] == 1u);

    for (auto k = count; k < output.size(); ++k)
        check(output[k] == sentinelBits);
};

auto tBuckets = test("Group/atomicAddReturnsThePreviousValueAndLoadReadsIt") = []
{
    constexpr auto count = 150;

    auto kernel = BucketKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto counts = makeUInts((int) bucketCount, 0u);
    auto output = makeUInts(count * 2, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.counts, counts);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
    {
        check(output[i * 2] == (std::uint32_t) i / bucketCount);
        check(output[i * 2 + 1] == 0u);
    }

    for (auto bucket = 0u; bucket < bucketCount; ++bucket)
        check(counts[(int) bucket]
              == (count - bucket + bucketCount - 1) / bucketCount);

    auto reader = LoadKernel {};
    auto load = Executor {reader};
    check(load.isValid(), load.reason());

    auto loaded = makeFloats(8, sentinel);
    auto loadBindings = Bindings {};
    loadBindings.set(reader.counts, counts);
    loadBindings.set(reader.output, loaded);
    check(load.dispatch(loadBindings, 8));

    for (auto k = 0; k < 8; ++k)
        check(loaded[k] == (k < (int) bucketCount ? (float) counts[k] : 0.f));
};

// ---------------------------------------------------------------------------
// The SIMD-group index: the flat lane over 32, whatever the shape.

namespace
{
constexpr auto simdTileWidth = 8;
constexpr auto simdTileHeight = 12;

struct SimdIndexKernel final : ComputeKernel
{
    SimdIndexKernel()
        : ComputeKernel({simdTileWidth, simdTileHeight})
    {
        compile();
    }

    void define() override
    {
        auto position = threadPosition();
        write(output, position.y * gridWidth() + position.x, simdGroupIndex());
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

auto tSimdIndex = test("Group/simdGroupIndexIsTheFlatLaneOverThirtyTwo") = []
{
    constexpr auto width = 2 * simdTileWidth;
    constexpr auto height = 2 * simdTileHeight;

    auto kernel = SimdIndexKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeUInts(width * height, sentinelBits);
    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, width, height));

    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            auto lane = (y % simdTileHeight) * simdTileWidth + x % simdTileWidth;
            check(output[y * width + x] == (std::uint32_t) (lane / simdGroupWidth));
        }
    }

    auto builder = ShaderBuilder {};
    builder.setThreadGroupShape({16});
    auto narrow = builder.uintOutputBuffer();
    builder.write(narrow, builder.threadId(), builder.simdGroupIndex());

    auto small = Executor {builder.graph()};
    check(small.isValid(), small.reason());

    auto indices = makeUInts(40, sentinelBits);
    auto smallBindings = Bindings {};
    smallBindings.set(narrow, indices);
    check(small.dispatch(smallBindings, 40));

    for (auto k = 0; k < 40; ++k)
        check(indices[k] == 0u);
};

auto tLargeGroup = test("Group/aGroupOfAThousandLanesFoldsAsOne") = []
{
    constexpr auto width = 1024;

    auto builder = ShaderBuilder {};
    builder.setThreadGroupShape({width});

    auto output = builder.uintOutputBuffer();
    auto lane = builder.localId();
    auto tile = builder.shared<UInt>(width);
    builder.write(tile, lane, lane * 3u);
    builder.barrier();
    auto total = builder.groupSum(tile[(unsigned) width - 1u - lane]);
    builder.write(output, builder.threadId(), total + builder.groupId());

    auto executor = Executor {builder.graph()};
    check(executor.isValid(), executor.reason());

    auto values = makeUInts(width * 2, sentinelBits);
    auto bindings = Bindings {};
    bindings.set(output, values);
    check(executor.dispatch(bindings, width * 2));

    constexpr auto expected = 3u * (width - 1u) * width / 2u;

    for (auto k = 0; k < width * 2; ++k)
        check(values[k] == expected + (std::uint32_t) (k / width));
};

// ---------------------------------------------------------------------------
// The indirect dispatch: group counts one CPU dispatch wrote, read by another.

namespace
{
constexpr auto candidateCount = 1000;
constexpr auto markedCount = 137;
constexpr auto capacity = 1024;

struct CountKernel final : ComputeKernel
{
    CountKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        ifThen(candidates[id] > 0.5f, [&] { atomicAdd(arguments, 3u, 1u); });
    }

    Uniform<InputBuffer> candidates;
    Uniform<AtomicBuffer> arguments;

    EACP_SHADER(candidates, arguments)
};

struct PrepareKernel final : ComputeKernel
{
    PrepareKernel() { compile(); }

    void define() override
    {
        auto width = (unsigned) ComputePass::threadGroupWidth;
        auto count = arguments.load(3u);

        write(arguments, 0u, (count + (width - 1u)) / width);
        write(arguments, 1u, 1u);
        write(arguments, 2u, 1u);
    }

    Uniform<AtomicBuffer> arguments;

    EACP_SHADER(arguments)
};

struct ConsumeKernel final : ComputeKernel
{
    ConsumeKernel() { compile(); }

    void define() override
    {
        auto one = constant(1.f);
        write(output, threadId(), one);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct GuardedConsumeKernel final : ComputeKernel
{
    GuardedConsumeKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto one = constant(1.f);

        ifThen(id < arguments.load(3u), [&] { write(output, id, one); });
    }

    Uniform<AtomicBuffer> arguments;
    Uniform<OutputBuffer> output;

    EACP_SHADER(arguments, output)
};

struct CountThreadsKernel final : ComputeKernel
{
    CountThreadsKernel() { compile(); }

    void define() override
    {
        atomicAdd(counter, 0u, 1u);
        atomicAdd(counter, 1u, threadId());
        atomicAdd(counter, 2u, gridCount());
    }

    Uniform<AtomicBuffer> counter;

    EACP_SHADER(counter)
};

int onesIn(const Vector<float>& values)
{
    auto ones = 0;

    for (auto value: values)
        ones += value == 1.f ? 1 : 0;

    return ones;
}

bool onesThenSentinels(const Vector<float>& values, int ones)
{
    for (auto k = 0; k < values.size(); ++k)
        if (values[k] != (k < ones ? 1.f : sentinel))
            return false;

    return true;
}
} // namespace

auto tIndirect =
    test("Group/anIndirectDispatchRunsTheGroupsAnotherDispatchWrote") = []
{
    auto candidates = makeFloats(candidateCount, 0.f);

    for (auto k = 0; k < markedCount; ++k)
        candidates[k * 7 + 3] = 1.f;

    auto arguments = makeUInts(4, 0u);

    auto count = CountKernel {};
    auto prepare = PrepareKernel {};
    auto consume = ConsumeKernel {};
    auto guarded = GuardedConsumeKernel {};

    auto runCount = Executor {count};
    auto runPrepare = Executor {prepare};
    auto runConsume = Executor {consume};
    auto runGuarded = Executor {guarded};

    check(runCount.isValid(), runCount.reason());
    check(runPrepare.isValid(), runPrepare.reason());
    check(runConsume.isValid(), runConsume.reason());
    check(runGuarded.isValid(), runGuarded.reason());

    auto countBindings = Bindings {};
    countBindings.set(count.candidates, candidates);
    countBindings.set(count.arguments, arguments);
    check(runCount.dispatch(countBindings, candidateCount));

    auto prepareBindings = Bindings {};
    prepareBindings.set(prepare.arguments, arguments);
    check(runPrepare.dispatch(prepareBindings, 1));

    auto groups = (markedCount + groupWidth - 1) / groupWidth;
    check(arguments[0] == (std::uint32_t) groups);
    check(arguments[1] == 1u);
    check(arguments[2] == 1u);
    check(arguments[3] == (std::uint32_t) markedCount);

    auto output = makeFloats(capacity, sentinel);
    auto consumeBindings = Bindings {};
    consumeBindings.set(consume.output, output);
    check(runConsume.dispatchIndirect(consumeBindings, arguments, capacity));
    check(onesIn(output) == groups * groupWidth);
    check(onesThenSentinels(output, groups * groupWidth));

    auto exact = makeFloats(capacity, sentinel);
    auto guardedBindings = Bindings {};
    guardedBindings.set(guarded.arguments, arguments);
    guardedBindings.set(guarded.output, exact);
    check(runGuarded.dispatchIndirect(guardedBindings, arguments, capacity));
    check(onesThenSentinels(exact, markedCount));

    auto clipped = makeFloats(capacity, sentinel);
    consumeBindings.set(consume.output, clipped);
    check(runConsume.dispatchIndirect(consumeBindings, arguments, 100));
    check(onesThenSentinels(clipped, 100));
};

auto tIndirectShape =
    test("Group/anIndirectDispatchReadsItsArgumentsAtTheOffset") = []
{
    auto kernel = CountThreadsKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto counter = makeUInts(3, 0u);
    auto bindings = Bindings {};
    bindings.set(kernel.counter, counter);

    auto reset = [&] { counter[0] = counter[1] = counter[2] = 0u; };

    auto arguments = std::array<std::uint32_t, 7> {9u, 9u, 9u, 7u, 2u, 3u, 1u};
    check(executor.dispatchIndirect(bindings, arguments, 100, 4));

    auto perRow = 0u;

    for (auto id = 0u; id < 100u; ++id)
        perRow += id;

    check(counter[0] == 300u);
    check(counter[1] == 3u * perRow);
    check(counter[2] == 300u * 100u);

    for (auto offset: {5, 7, 100, -1})
    {
        reset();
        check(executor.dispatchIndirect(bindings, arguments, 100, offset));
        check(counter[0] == 0u);
    }

    auto empty = std::array<std::uint32_t, 3> {4u, 0u, 1u};
    check(executor.dispatchIndirect(bindings, empty, 100));
    check(counter[0] == 0u);

    check(executor.dispatchIndirect(bindings, std::span<const std::uint32_t> {}, 8));
    check(counter[0] == 0u);

    auto unbound = Bindings {};
    auto one = std::array<std::uint32_t, 3> {1u, 1u, 1u};
    check(!executor.dispatchIndirect(unbound, one, 8));

    auto grid = TileIdsKernel {};
    auto gridExecutor = Executor {grid};
    auto gridOutput = makeUInts(16, sentinelBits);
    auto gridBindings = Bindings {};
    gridBindings.set(grid.output, gridOutput);
    check(!gridExecutor.dispatchIndirect(gridBindings, one, 8));
    check(gridOutput[0] == sentinelBits);
};

// ---------------------------------------------------------------------------
// Refusals of what the typed builder cannot record, each with its reason.

namespace
{
bool refusedNaming(const Executor& executor, const char* name)
{
    return !executor.isValid() && executor.reason().find(name) != std::string::npos;
}
} // namespace

auto tGroupRefusals = test("Group/malformedGroupStatementsAreRefusedByName") = []
{
    {
        auto graph = ShaderGraph {};
        auto plain = graph.addStorageBuffer(BufferAccess::Write, ValueType::UInt);
        auto index = graph.addThreadId();
        auto one = graph.addUIntConstant(1u);
        graph.addAtomicAdd(plain, index, one);
        check(refusedNaming(Executor {graph}, "not atomic"));
    }

    {
        auto graph = ShaderGraph {};
        auto plain = graph.addStorageBuffer(BufferAccess::Write, ValueType::UInt);
        auto index = graph.addThreadId();
        graph.addStore(plain, index, graph.addAtomicLoad(plain, index));
        check(refusedNaming(Executor {graph}, "AtomicLoad"));
    }

    {
        auto graph = ShaderGraph {};
        auto output = graph.addStorageBuffer(BufferAccess::Write, ValueType::Float);
        auto index = graph.addThreadId();
        auto tile = graph.addSharedArray(ValueType::Float, 8);
        graph.addSharedStore(tile, index, graph.addUIntConstant(3u));
        graph.addStore(output, index, graph.addSharedRead(tile, index));
        check(refusedNaming(Executor {graph}, "SharedStore"));
    }

    {
        auto graph = ShaderGraph {};
        auto output = graph.addStorageBuffer(BufferAccess::Write, ValueType::Float);
        auto index = graph.addThreadId();
        auto pair = graph.addConstruct(
            ValueType::Float2, {graph.addConstant(1.f), graph.addConstant(2.f)});
        auto folded =
            graph.addGroupReduction(GroupReduction::Sum, ValueType::Float2, pair);
        graph.addStore(
            output,
            index,
            graph.addSwizzle(ValueType::Float, graph.addVarRead(folded), "x"));
        check(refusedNaming(Executor {graph}, "GroupReduce"));
    }

    {
        auto graph = ShaderGraph {};
        auto input = graph.addStorageBuffer(BufferAccess::Read, ValueType::Float);
        auto zero = graph.addUIntConstant(0u);
        auto eight = graph.addUIntConstant(8u);
        auto fragment =
            graph.addSimdMatrixLoad(SimdMatrixMemory::Buffer, input, zero, eight);
        graph.addSimdMatrixStore(
            fragment, SimdMatrixMemory::Buffer, input, zero, eight);
        check(
            refusedNaming(Executor {graph}, "SimdMatrixStore writes storage slot"));
    }

    {
        auto graph = ShaderGraph {};
        auto words = graph.addStorageBuffer(BufferAccess::Write, ValueType::UInt);
        auto zero = graph.addUIntConstant(0u);
        auto eight = graph.addUIntConstant(8u);
        auto fragment =
            graph.addSimdMatrixLoad(SimdMatrixMemory::Buffer, words, zero, eight);
        graph.addSimdMatrixStore(
            fragment, SimdMatrixMemory::Buffer, words, zero, eight);
        check(refusedNaming(Executor {graph}, "does not hold Float"));
    }

    {
        auto graph = ShaderGraph {};
        auto output = graph.addStorageBuffer(BufferAccess::Write, ValueType::Float);
        auto eight = graph.addUIntConstant(8u);
        auto fragment = graph.addSimdMatrixFill(graph.addConstant(1.f));
        graph.addSimdMatrixStore(fragment,
                                 SimdMatrixMemory::Buffer,
                                 output,
                                 graph.addConstant(0.f),
                                 eight);
        check(
            refusedNaming(Executor {graph}, "offset that is not a scalar integer"));
    }
};
