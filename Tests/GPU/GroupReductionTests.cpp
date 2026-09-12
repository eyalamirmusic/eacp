#include "Common.h"

#include <cmath>
#include <vector>

// What a group reduction has to answer with on a device: the fold over the
// whole group, the same number on every thread of it, whatever else the kernel
// does before or after.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto groupSize = ComputePass::threadGroupWidth;
constexpr auto groups = 3;
constexpr auto threadCount = groupSize * groups;

// Distinctive per lane: not monotonic, negative in places, and with the
// extremes off the ends - lane 7 holds the minimum and the last lane of the
// group the maximum, so a fold that dropped either half is a different number.
float laneValue(int lane)
{
    if (lane == 7)
        return -91.5f;

    if (lane == groupSize - 1)
        return 137.25f;

    return (float) ((lane * 13) % 29) - 11.f;
}

float groupTotal()
{
    auto total = 0.f;

    for (auto lane = 0; lane < groupSize; ++lane)
        total += laneValue(lane);

    return total;
}

std::vector<float> laneValues()
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < threadCount; ++i)
        values.push_back(laneValue(i % groupSize));

    return values;
}

// Every thread writes the three folds of its own group, so a result that
// reached only some of them shows up as a slot that disagrees with its
// neighbours.
struct FoldKernel final : ComputeProgram
{
    FoldKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto value = input[id];

        auto total = groupSum(value);
        auto peak = groupMax(value);
        auto least = groupMin(value);

        ifThen(id < gridCount(),
               [&]
               {
                   write(sums, id, total);
                   write(maxima, id, peak);
                   write(minima, id, least);
               });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> sums;
    Uniform<OutputBuffer> maxima;
    Uniform<OutputBuffer> minima;

    EACP_SHADER(input, sums, maxima, minima)
};

// The layernorm shape: a mean out of one fold, a variance out of a second one
// computed from it, and the normalised value written per thread.
struct MeanAndVarianceKernel final : ComputeProgram
{
    MeanAndVarianceKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto value = input[id];

        auto mean = groupSum(value) / (float) groupSize;
        auto centred = value - mean;
        auto variance = groupSum(centred * centred) / (float) groupSize;

        ifThen(id < gridCount(),
               [&]
               {
                   write(means, id, mean);
                   write(variances, id, variance);
               });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> means;
    Uniform<OutputBuffer> variances;

    EACP_SHADER(input, means, variances)
};

// A fold per row, inside the loop that walks the rows: the reduction is reached
// by every thread on every iteration, and its result changes with each one.
struct RowSumKernel final : ComputeProgram
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
                     groupSum(input[row.get() * (unsigned) groupSize + lane]);

                 ifThen(lane == 0u, [&] { write(output, row.get(), total); });
                 row = row.get() + 1u;
             });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UInt> rows;

    EACP_SHADER(input, output, rows)
};

// A 2D kernel folds over its whole 8x8 group rather than over one of its rows.
struct TileSumKernel final : ComputeProgram
{
    TileSumKernel() { compile(); }

    void define() override
    {
        auto p = threadPosition();
        auto index = p.y * gridWidth() + p.x;

        auto total = groupSum(input[index]);

        ifThen(p.x < gridWidth() && p.y < gridHeight(),
               [&] { write(output, index, total); });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// The unsigned siblings, folding the integer buffer a counting kernel leaves.
struct UIntFoldKernel final : ComputeProgram
{
    UIntFoldKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto value = input[id];

        auto total = groupSum(value);
        auto peak = groupMax(value);
        auto least = groupMin(value);

        ifThen(id < gridCount(),
               [&] { write(output, id, uint3(total, peak, least)); });
    }

    Uniform<UIntInputBuffer> input;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(input, output)
};

// The SIMD-scoped folds, over a group of four SIMD groups so that a fold which
// reached the whole group instead of one of them is a different number in every
// slot. Each SIMD group's values are offset by a hundred from the one before,
// which puts the four sums, maxima and minima a long way apart.
constexpr auto simdWidth = ComputeProgram::simdWidth;
constexpr auto simdGroupsPerGroup = 4;
constexpr auto simdGroupThreads = simdWidth * simdGroupsPerGroup;
constexpr auto simdThreadCount = simdGroupThreads * 2;

float simdLaneValue(int index)
{
    auto block = index / simdWidth;
    auto lane = index % simdWidth;

    return (float) ((lane * 7) % 23) - 9.f + (float) block * 100.f;
}

float simdBlockTotal(int block)
{
    auto total = 0.f;

    for (auto lane = 0; lane < simdWidth; ++lane)
        total += simdLaneValue(block * simdWidth + lane);

    return total;
}

std::vector<float> simdLaneValues()
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < simdThreadCount; ++i)
        values.push_back(simdLaneValue(i));

    return values;
}

struct SimdFoldKernel final : ComputeProgram
{
    SimdFoldKernel()
        : ComputeProgram({simdGroupThreads, 1, 1})
    {
        compile();
    }

    void define() override
    {
        auto id = threadId();
        auto value = input[id];

        auto total = simdSum(value);
        auto peak = simdMax(value);
        auto least = simdMin(value);

        ifThen(id < gridCount(),
               [&]
               {
                   write(sums, id, total);
                   write(maxima, id, peak);
                   write(minima, id, least);
               });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> sums;
    Uniform<OutputBuffer> maxima;
    Uniform<OutputBuffer> minima;

    EACP_SHADER(input, sums, maxima, minima)
};

// Both scopes over the same values, which is the relation between them: the
// whole group's sum is the four SIMD groups' sums added up.
struct BothScopesKernel final : ComputeProgram
{
    BothScopesKernel()
        : ComputeProgram({simdGroupThreads, 1, 1})
    {
        compile();
    }

    void define() override
    {
        auto id = threadId();
        auto value = input[id];

        auto whole = groupSum(value);
        auto narrow = simdSum(value);

        ifThen(id < gridCount(),
               [&]
               {
                   write(wholeGroup, id, whole);
                   write(perSimdGroup, id, narrow);
               });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> wholeGroup;
    Uniform<OutputBuffer> perSimdGroup;

    EACP_SHADER(input, wholeGroup, perSimdGroup)
};

// The unsigned sibling of the narrow fold, on the terms UIntFoldKernel sets.
struct UIntSimdFoldKernel final : ComputeProgram
{
    UIntSimdFoldKernel()
        : ComputeProgram({simdGroupThreads, 1, 1})
    {
        compile();
    }

    void define() override
    {
        auto id = threadId();
        auto value = input[id];

        auto total = simdSum(value);
        auto peak = simdMax(value);
        auto least = simdMin(value);

        ifThen(id < gridCount(),
               [&] { write(output, id, uint3(total, peak, least)); });
    }

    Uniform<UIntInputBuffer> input;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(input, output)
};

// A group of exactly simdWidth threads, which is where the two scopes fold the
// same set of threads on paper and where the emitter used to be tempted to
// answer the wide one with a bare intrinsic. What makes that wrong is that the
// hardware SIMD group is not necessarily simdWidth threads - an Intel Mac runs
// a kernel at eight or sixteen - so both folds run here and both are checked.
constexpr auto narrowGroupThreads = simdWidth;
constexpr auto narrowGroupCount = 3;
constexpr auto narrowThreadCount = narrowGroupThreads * narrowGroupCount;

struct NarrowGroupKernel final : ComputeProgram
{
    NarrowGroupKernel()
        : ComputeProgram({narrowGroupThreads, 1, 1})
    {
        compile();
    }

    void define() override
    {
        auto id = threadId();
        auto value = input[id];

        auto whole = groupSum(value);
        auto narrow = simdSum(value);
        auto peak = groupMax(value);

        ifThen(id < gridCount(),
               [&]
               {
                   write(wholeGroup, id, whole);
                   write(perSimdGroup, id, narrow);
                   write(maxima, id, peak);
               });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> wholeGroup;
    Uniform<OutputBuffer> perSimdGroup;
    Uniform<OutputBuffer> maxima;

    EACP_SHADER(input, wholeGroup, perSimdGroup, maxima)
};

Buffer makeFloatBuffer(Device& device, const std::vector<float>& values)
{
    return device.makeBuffer(
        values.data(), (int) (sizeof(float) * values.size()), BufferUsage::Storage);
}
} // namespace

// The sum, the maximum and the minimum of a group, on every thread of it.
auto tGroupFoldsAreRight = test("GroupReduction/sumMaxAndMinOverTheGroup") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = laneValues();
    auto input = makeFloatBuffer(device, values);

    auto bytes = sizeof(float) * threadCount;
    auto sums = device.makeBuffer(bytes, BufferUsage::Storage);
    auto maxima = device.makeBuffer(bytes, BufferUsage::Storage);
    auto minima = device.makeBuffer(bytes, BufferUsage::Storage);

    auto kernel = FoldKernel {};
    kernel.input = input;
    kernel.sums = sums;
    kernel.maxima = maxima;
    kernel.minima = minima;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, threadCount);
        }

        commands.commit();
    }

    auto readBack = [&](Buffer& buffer)
    {
        auto out = std::vector<float>(threadCount, 0.f);
        buffer.read(out.data(), bytes);
        return out;
    };

    auto total = groupTotal();
    auto summed = readBack(sums);
    auto peaks = readBack(maxima);
    auto least = readBack(minima);

    auto agreeing = 0;

    for (auto i = 0; i < threadCount; ++i)
        if (std::abs(summed[i] - total) < 1.0e-3f && peaks[i] == 137.25f
            && least[i] == -91.5f)
            ++agreeing;

    // Every slot, so a fold that reached only the first lane of the group - or
    // only the first group - is a count short of this.
    check(agreeing == threadCount);

    // And bit for bit the same number on every thread of a group, which is
    // what "returned to every thread" means rather than "close enough".
    auto identical = 0;

    for (auto i = 0; i < threadCount; ++i)
        if (summed[i] == summed[(i / groupSize) * groupSize])
            ++identical;

    check(identical == threadCount);
};

// A mean out of one fold and a variance out of a second, which is what a
// layernorm is written out of.
auto tTwoFoldsInOneKernel = test("GroupReduction/aMeanThenAVariance") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = laneValues();
    auto input = makeFloatBuffer(device, values);

    auto bytes = sizeof(float) * threadCount;
    auto means = device.makeBuffer(bytes, BufferUsage::Storage);
    auto variances = device.makeBuffer(bytes, BufferUsage::Storage);

    auto kernel = MeanAndVarianceKernel {};
    kernel.input = input;
    kernel.means = means;
    kernel.variances = variances;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, threadCount);
        }

        commands.commit();
    }

    auto expectedMean = groupTotal() / (float) groupSize;
    auto expectedVariance = 0.f;

    for (auto lane = 0; lane < groupSize; ++lane)
    {
        auto centred = laneValue(lane) - expectedMean;
        expectedVariance += centred * centred;
    }

    expectedVariance /= (float) groupSize;

    auto meansBack = std::vector<float>(threadCount, 0.f);
    auto variancesBack = std::vector<float>(threadCount, 0.f);
    means.read(meansBack.data(), bytes);
    variances.read(variancesBack.data(), bytes);

    auto agreeing = 0;

    for (auto i = 0; i < threadCount; ++i)
        if (std::abs(meansBack[i] - expectedMean) < 1.0e-3f
            && std::abs(variancesBack[i] - expectedVariance) < 1.0e-1f)
            ++agreeing;

    check(agreeing == threadCount);
};

// A fold inside a loop body, once per row.
auto tFoldInsideALoop = test("GroupReduction/aFoldPerLoopIteration") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 5;

    auto values = std::vector<float> {};

    for (auto row = 0; row < rows; ++row)
        for (auto lane = 0; lane < groupSize; ++lane)
            values.push_back((float) (row + 1) * laneValue(lane));

    auto input = makeFloatBuffer(device, values);
    auto output = device.makeBuffer(sizeof(float) * rows, BufferUsage::Storage);

    auto kernel = RowSumKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.rows = (unsigned) rows;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, groupSize);
        }

        commands.commit();
    }

    auto back = std::vector<float>(rows, 0.f);
    output.read(back.data(), sizeof(float) * rows);

    auto correct = 0;

    for (auto row = 0; row < rows; ++row)
        if (std::abs(back[row] - (float) (row + 1) * groupTotal()) < 1.0e-2f)
            ++correct;

    check(correct == rows);
};

// A 2D kernel reduces over the whole 8x8 group.
auto tTwoDimensionalFold = test("GroupReduction/aTwoDGroupFoldsAllOfIt") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto tile = ComputePass::threadGroupSize2D;
    constexpr auto width = tile * 2;
    constexpr auto height = tile * 2;
    constexpr auto count = width * height;

    auto values = std::vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.push_back((float) (i % 7) + 0.5f);

    auto input = makeFloatBuffer(device, values);
    auto output = device.makeBuffer(sizeof(float) * count, BufferUsage::Storage);

    auto kernel = TileSumKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, width, height);
        }

        commands.commit();
    }

    auto back = std::vector<float>(count, 0.f);
    output.read(back.data(), sizeof(float) * count);

    auto correct = 0;

    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            auto expected = 0.f;

            for (auto row = 0; row < tile; ++row)
                for (auto column = 0; column < tile; ++column)
                    expected += values[(size_t) ((y / tile) * tile + row) * width
                                       + (size_t) ((x / tile) * tile + column)];

            if (std::abs(back[(size_t) y * width + (size_t) x] - expected) < 1.0e-2f)
                ++correct;
        }
    }

    check(correct == count);
};

// The unsigned siblings fold the same way.
auto tUnsignedFolds = test("GroupReduction/theUnsignedSiblingsFold") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = std::vector<std::uint32_t> {};

    for (auto i = 0; i < threadCount; ++i)
    {
        auto lane = i % groupSize;
        values.push_back(lane == 5 ? 4000u
                                   : (std::uint32_t) ((lane * 17) % 31) + 3u);
    }

    auto input = device.makeBuffer(values.data(),
                                   (int) (sizeof(std::uint32_t) * threadCount),
                                   BufferUsage::Storage);

    auto output = device.makeBuffer(sizeof(std::uint32_t) * threadCount * 3,
                                    BufferUsage::Storage);

    auto kernel = UIntFoldKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, threadCount);
        }

        commands.commit();
    }

    auto expectedSum = std::uint32_t {0};
    auto expectedMax = std::uint32_t {0};
    auto expectedMin = std::uint32_t {~0u};

    for (auto lane = 0; lane < groupSize; ++lane)
    {
        expectedSum += values[(size_t) lane];
        expectedMax = std::max(expectedMax, values[(size_t) lane]);
        expectedMin = std::min(expectedMin, values[(size_t) lane]);
    }

    auto back = std::vector<std::uint32_t>((size_t) threadCount * 3, 0u);
    output.read(back.data(), sizeof(std::uint32_t) * back.size());

    auto agreeing = 0;

    for (auto i = 0; i < threadCount; ++i)
        if (back[(size_t) i * 3] == expectedSum
            && back[(size_t) i * 3 + 1] == expectedMax
            && back[(size_t) i * 3 + 2] == expectedMin)
            ++agreeing;

    check(agreeing == threadCount);
};

// The narrow fold: every thread holds the fold of the thirty-two threads it
// shares a SIMD group with, and the four SIMD groups of a threadgroup come out
// holding four different numbers.
auto tSimdFoldsAreRight = test("GroupReduction/sumMaxAndMinOverOneSimdGroup") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = simdLaneValues();
    auto input = makeFloatBuffer(device, values);

    auto bytes = sizeof(float) * simdThreadCount;
    auto sums = device.makeBuffer((int) bytes, BufferUsage::Storage);
    auto maxima = device.makeBuffer((int) bytes, BufferUsage::Storage);
    auto minima = device.makeBuffer((int) bytes, BufferUsage::Storage);

    auto kernel = SimdFoldKernel {};
    kernel.input = input;
    kernel.sums = sums;
    kernel.maxima = maxima;
    kernel.minima = minima;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, simdThreadCount);
        }

        commands.commit();
    }

    auto readBack = [&](Buffer& buffer)
    {
        auto out = std::vector<float>(simdThreadCount, 0.f);
        buffer.read(out.data(), (int) bytes);
        return out;
    };

    auto summed = readBack(sums);
    auto peaks = readBack(maxima);
    auto least = readBack(minima);

    auto agreeing = 0;

    for (auto i = 0; i < simdThreadCount; ++i)
    {
        auto block = i / simdWidth;

        auto expectedMax = simdLaneValue(block * simdWidth);
        auto expectedMin = expectedMax;

        for (auto lane = 1; lane < simdWidth; ++lane)
        {
            auto value = simdLaneValue(block * simdWidth + lane);
            expectedMax = std::max(expectedMax, value);
            expectedMin = std::min(expectedMin, value);
        }

        if (std::abs(summed[(size_t) i] - simdBlockTotal(block)) < 1.0e-2f
            && peaks[(size_t) i] == expectedMax && least[(size_t) i] == expectedMin)
            ++agreeing;
    }

    check(agreeing == simdThreadCount);

    // And the four SIMD groups of a threadgroup disagree with each other, which
    // is what says the fold stopped at a SIMD group rather than running on.
    auto distinct = 0;

    for (auto block = 1; block < simdGroupsPerGroup; ++block)
        if (summed[(size_t) block * simdWidth] != summed[0])
            ++distinct;

    check(distinct == simdGroupsPerGroup - 1);
};

// The relation between the two scopes: a group's sum is its SIMD groups' sums
// added up, which is the same arithmetic reached two ways in one kernel.
auto tScopesAgree = test("GroupReduction/theWideFoldIsTheNarrowOnesAddedUp") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = simdLaneValues();
    auto input = makeFloatBuffer(device, values);

    auto bytes = sizeof(float) * simdThreadCount;
    auto wholeGroup = device.makeBuffer((int) bytes, BufferUsage::Storage);
    auto perSimdGroup = device.makeBuffer((int) bytes, BufferUsage::Storage);

    auto kernel = BothScopesKernel {};
    kernel.input = input;
    kernel.wholeGroup = wholeGroup;
    kernel.perSimdGroup = perSimdGroup;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, simdThreadCount);
        }

        commands.commit();
    }

    auto wide = std::vector<float>(simdThreadCount, 0.f);
    auto narrow = std::vector<float>(simdThreadCount, 0.f);
    wholeGroup.read(wide.data(), (int) bytes);
    perSimdGroup.read(narrow.data(), (int) bytes);

    auto agreeing = 0;

    for (auto i = 0; i < simdThreadCount; ++i)
    {
        auto firstBlock = (i / simdGroupThreads) * simdGroupsPerGroup;
        auto expectedWide = 0.f;

        for (auto block = 0; block < simdGroupsPerGroup; ++block)
            expectedWide += simdBlockTotal(firstBlock + block);

        if (std::abs(wide[(size_t) i] - expectedWide) < 1.0e-1f
            && std::abs(narrow[(size_t) i] - simdBlockTotal(i / simdWidth))
                   < 1.0e-2f)
            ++agreeing;
    }

    check(agreeing == simdThreadCount);
};

// The unsigned siblings, which take the same path with a scratch array of their
// own where the fold goes through one.
auto tUIntSimdFolds = test("GroupReduction/theUnsignedNarrowFold") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = std::vector<std::uint32_t> {};

    for (auto i = 0; i < simdThreadCount; ++i)
        values.push_back((std::uint32_t) (((i % simdWidth) * 11) % 37)
                         + (std::uint32_t) (i / simdWidth) * 1000u);

    auto input = device.makeBuffer(values.data(),
                                   (int) (sizeof(std::uint32_t) * values.size()),
                                   BufferUsage::Storage);

    auto output = device.makeBuffer(
        (int) (sizeof(std::uint32_t) * simdThreadCount * 3), BufferUsage::Storage);

    auto kernel = UIntSimdFoldKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, simdThreadCount);
        }

        commands.commit();
    }

    auto back = std::vector<std::uint32_t>((size_t) simdThreadCount * 3, 0u);
    output.read(back.data(), (int) (sizeof(std::uint32_t) * back.size()));

    auto agreeing = 0;

    for (auto i = 0; i < simdThreadCount; ++i)
    {
        auto block = i / simdWidth;

        auto expectedSum = std::uint32_t {0};
        auto expectedMax = std::uint32_t {0};
        auto expectedMin = std::uint32_t {~0u};

        for (auto lane = 0; lane < simdWidth; ++lane)
        {
            auto value = values[(size_t) (block * simdWidth + lane)];
            expectedSum += value;
            expectedMax = std::max(expectedMax, value);
            expectedMin = std::min(expectedMin, value);
        }

        if (back[(size_t) i * 3] == expectedSum
            && back[(size_t) i * 3 + 1] == expectedMax
            && back[(size_t) i * 3 + 2] == expectedMin)
            ++agreeing;
    }

    check(agreeing == simdThreadCount);
};

// A group of exactly simdWidth threads, folded both ways against a CPU
// reference. The wide fold has to be right whatever the hardware SIMD group
// turns out to be, which is what the combine through the scratch is for; the
// narrow one is the same set of threads here, so on a device whose SIMD groups
// really are simdWidth wide the two agree to the bit.
auto tNarrowGroupFoldsBothWays =
    test("GroupReduction/aGroupOfOneSimdGroupFoldsBothWays") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = std::vector<float> {};

    for (auto i = 0; i < narrowThreadCount; ++i)
        values.push_back(simdLaneValue(i));

    auto input = makeFloatBuffer(device, values);

    auto bytes = sizeof(float) * narrowThreadCount;
    auto wholeGroup = device.makeBuffer((int) bytes, BufferUsage::Storage);
    auto perSimdGroup = device.makeBuffer((int) bytes, BufferUsage::Storage);
    auto maxima = device.makeBuffer((int) bytes, BufferUsage::Storage);

    auto kernel = NarrowGroupKernel {};
    kernel.input = input;
    kernel.wholeGroup = wholeGroup;
    kernel.perSimdGroup = perSimdGroup;
    kernel.maxima = maxima;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, narrowThreadCount);
        }

        commands.commit();
    }

    auto readBack = [&](Buffer& buffer)
    {
        auto out = std::vector<float>(narrowThreadCount, 0.f);
        buffer.read(out.data(), (int) bytes);
        return out;
    };

    auto wide = readBack(wholeGroup);
    auto narrow = readBack(perSimdGroup);
    auto peaks = readBack(maxima);

    auto agreeing = 0;

    for (auto i = 0; i < narrowThreadCount; ++i)
    {
        auto block = i / narrowGroupThreads;
        auto expected = simdBlockTotal(block);

        auto expectedMax = simdLaneValue(block * narrowGroupThreads);

        for (auto lane = 1; lane < narrowGroupThreads; ++lane)
            expectedMax = std::max(expectedMax,
                                   simdLaneValue(block * narrowGroupThreads + lane));

        if (std::abs(wide[(size_t) i] - expected) < 1.0e-2f
            && std::abs(narrow[(size_t) i] - expected) < 1.0e-2f
            && peaks[(size_t) i] == expectedMax)
            ++agreeing;
    }

    check(agreeing == narrowThreadCount);
};
