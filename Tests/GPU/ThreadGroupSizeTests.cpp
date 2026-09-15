#include "Common.h"

// The threadgroup a kernel asked for, on the device.
//
// Every check here is one a group of the stock size would fail: a 256-lane sum
// covers four times what 64 lanes do, a 16x16 tile transposes a block an 8x8
// group cannot reach, and a local id runs to the width its own kernel named.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto wideGroup = 256;
constexpr auto tile = 16;

// 256 lanes fold a 256-element shared tile down to element zero. A group of 64
// would sum a quarter of each slice, so the numbers say how many threads the
// group really had.
struct WideSumKernel final : ComputeProgram
{
    WideSumKernel()
        : ComputeProgram({wideGroup})
    {
        compile();
    }

    void define() override
    {
        auto gid = threadId();
        auto lane = localId();
        auto lanes = groupShape().x;
        auto scratch = shared<Float>(lanes);

        auto value = var(0.0f);
        ifThen(gid < gridCount(), [&] { value = input[gid]; });
        write(scratch, lane, value.get());
        barrier();

        for (auto stride = lanes / 2; stride > 0; stride /= 2)
        {
            auto bound = (unsigned) stride;

            ifThen(lane < bound,
                   [&]
                   { write(scratch, lane, scratch[lane] + scratch[lane + bound]); });
            barrier();
        }

        ifThen(lane == 0u, [&] { write(output, groupId(), scratch[0u]); });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// A 16x16 group transposes its own tile through threadgroup memory: the tile is
// read in row order and written back in column order, at the group's own
// transposed origin.
struct TileTransposeKernel final : ComputeProgram
{
    TileTransposeKernel()
        : ComputeProgram({tile, tile})
    {
        compile();
    }

    void define() override
    {
        auto p = threadPosition();
        auto local = localPosition();
        auto group = groupPosition();
        auto side = (unsigned) groupShape().x;
        auto scratch = shared<Float>(tile * tile);

        write(scratch, local.y * side + local.x, input[p.y * gridWidth() + p.x]);
        barrier();

        auto x = group.y * side + local.x;
        auto y = group.x * side + local.y;

        write(output, y * gridWidth() + x, scratch[local.x * side + local.y]);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// What a thread's own place in its group is, which is the group's width counted
// out one lane at a time.
struct LocalIdKernel final : ComputeProgram
{
    LocalIdKernel() { compile(); }

    void define() override { write(output, threadId(), toFloat(localId())); }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct WideLocalIdKernel final : ComputeProgram
{
    WideLocalIdKernel()
        : ComputeProgram({wideGroup})
    {
        compile();
    }

    void define() override { write(output, threadId(), toFloat(localId())); }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

// The 2D sibling, reporting both axes of the local position at once.
struct LocalPositionKernel final : ComputeProgram
{
    LocalPositionKernel() { compile(); }

    void define() override
    {
        auto p = threadPosition();
        auto local = localPosition();

        write(output,
              p.y * gridWidth() + p.x,
              toFloat(local.x) + toFloat(local.y) * 1000.f);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

// The count the consuming kernel's own group width divides, which is what an
// indirect dispatch reads as a threadgroup count.
struct PrepareWideKernel final : ComputeProgram
{
    PrepareWideKernel() { compile(); }

    void define() override
    {
        auto width = (unsigned) wideGroup;
        auto count = arguments.load(3u);

        write(arguments, 0u, (count + (width - 1u)) / width);
        write(arguments, 1u, 1u);
        write(arguments, 2u, 1u);
    }

    Uniform<AtomicBuffer> arguments;

    EACP_SHADER(arguments)
};

// Unguarded on purpose: what has to be observable is how many threads ran, and
// a kernel that guards itself writes the same output however large the grid was.
struct WideConsumeKernel final : ComputeProgram
{
    WideConsumeKernel()
        : ComputeProgram({wideGroup})
    {
        compile();
    }

    void define() override { write(output, threadId(), constant(1.f)); }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

constexpr auto untouched = -1.f;

Buffer makeOutput(int count)
{
    auto blank = Vector<float> {};
    blank.assign(count, untouched);

    return Buffer {Device::shared(),
                   blank.data(),
                   (int) sizeof(float) * count,
                   BufferUsage::Storage};
}

Vector<float> readAll(const Buffer& buffer, int count)
{
    auto values = Vector<float> {};
    values.resize(count);
    buffer.read(values.data(), (int) sizeof(float) * count);
    return values;
}

template <typename Kernel>
Vector<float> localIdsOf(Kernel& kernel, int count)
{
    auto output = makeOutput(count);
    kernel.output = output;
    kernel.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();
    return readAll(output, count);
}
} // namespace

// Each group sums the 256 elements it owns, which is four times what the stock
// group would have reached.
auto tWideGroupSums = test("ThreadGroupSize/aWideGroupSumsItsOwnRun") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto groups = 3;
    constexpr auto count = wideGroup * groups;

    auto values = Vector<float> {};
    values.resize(count);

    for (auto i = 0; i < count; ++i)
        values[i] = (float) (i % 7 + 1);

    auto input = device.makeBuffer(
        values.data(), (int) sizeof(float) * count, BufferUsage::Storage);
    auto sums = makeOutput(groups);

    auto kernel = WideSumKernel {};
    kernel.input = input;
    kernel.output = sums;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, count);
        }

        commands.commit();
    }

    auto result = readAll(sums, groups);

    for (auto group = 0; group < groups; ++group)
    {
        auto expected = 0.f;

        for (auto i = group * wideGroup; i < (group + 1) * wideGroup; ++i)
            expected += values[i];

        check(result[group] == expected);
    }
};

// A 16x16 group, checked by the one arrangement of its tile that only a group
// of that shape produces.
auto tTileGroupTransposes =
    test("ThreadGroupSize/aTiledGroupTransposesItsBlock") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto side = tile * 2;
    constexpr auto cells = side * side;

    auto values = Vector<float> {};
    values.resize(cells);

    for (auto y = 0; y < side; ++y)
        for (auto x = 0; x < side; ++x)
            values[y * side + x] = (float) (y * side + x);

    auto input = device.makeBuffer(
        values.data(), (int) sizeof(float) * cells, BufferUsage::Storage);
    auto output = makeOutput(cells);

    auto kernel = TileTransposeKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, side, side);
        }

        commands.commit();
    }

    auto result = readAll(output, cells);
    auto correct = 0;

    for (auto y = 0; y < side; ++y)
        for (auto x = 0; x < side; ++x)
            if (result[y * side + x] == values[x * side + y])
                ++correct;

    check(correct == cells);

    // And genuinely transposed rather than copied, which the diagonal alone
    // would not have told apart.
    check(result[1] != values[1]);
};

// The width a lane counts to is its own kernel's, and a kernel that named no
// shape still counts to the stock one.
auto tLocalIdRunsToTheGroupWidth =
    test("ThreadGroupSize/aLaneCountsToItsOwnGroupWidth") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = wideGroup * 2;

    auto stock = LocalIdKernel {};
    auto stockIds = localIdsOf(stock, count);

    auto wide = WideLocalIdKernel {};
    auto wideIds = localIdsOf(wide, count);

    auto stockCorrect = 0;
    auto wideCorrect = 0;

    for (auto i = 0; i < count; ++i)
    {
        if (stockIds[i] == (float) (i % ComputeProgram::groupWidth))
            ++stockCorrect;

        if (wideIds[i] == (float) (i % wideGroup))
            ++wideCorrect;
    }

    check(stockCorrect == count);
    check(wideCorrect == count);
};

// The 2D default, unchanged by any of this: 8x8.
auto tStockGridGroupIsEightSquared =
    test("ThreadGroupSize/theStockGridGroupIsEightSquared") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto side = 32;
    constexpr auto cells = side * side;

    auto output = makeOutput(cells);

    auto kernel = LocalPositionKernel {};
    kernel.output = output;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, side, side);
        }

        commands.commit();
    }

    auto result = readAll(output, cells);
    auto correct = 0;

    constexpr auto stock = ComputeProgram::groupSize2D;

    for (auto y = 0; y < side; ++y)
        for (auto x = 0; x < side; ++x)
            if (result[y * side + x]
                == (float) (x % stock) + (float) (y % stock) * 1000.f)
                ++correct;

    check(correct == cells);
};

// The indirect path takes the group from the pipeline it is dispatching, so a
// grid of n groups runs n * 256 threads and nothing else produces that number.
auto tIndirectDispatchUsesTheProgramsGroup =
    test("ThreadGroupSize/anIndirectDispatchRunsTheProgramsGroup") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto capacity = 1024;
    constexpr auto marked = 300;

    std::uint32_t initial[] = {0u, 1u, 1u, (std::uint32_t) marked};

    auto arguments =
        device.makeBuffer(initial, sizeof(initial), BufferUsage::Storage);
    auto output = makeOutput(capacity);

    auto prepare = PrepareWideKernel {};
    prepare.arguments = arguments;
    prepare.prepare();

    auto consume = WideConsumeKernel {};
    consume.output = output;
    consume.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(prepare, 1);
        }

        {
            auto pass = commands.beginCompute();
            pass.dispatchIndirect(consume, arguments, capacity);
        }

        commands.commit();
    }

    auto values = readAll(output, capacity);
    auto written = 0;

    for (auto value: values)
        if (value != untouched)
            ++written;

    constexpr auto groups = (marked + wideGroup - 1) / wideGroup;
    check(written == groups * wideGroup);
};
