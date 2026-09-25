#include "CpuCrossCheck.h"

// The threadgroup a kernel asked for, on the device.
//
// Every check here is one a group of the stock size would fail: a 256-lane sum
// covers four times what 64 lanes do, a 16x16 tile transposes a block an 8x8
// group cannot reach, and a local id runs to the width its own kernel named.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

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
void expectLocalIdsWrap(Kernel& kernel, int count, int width)
{
    CrossCheck {kernel}
        .output(kernel.output, count, untouched)
        .agreeing()
        .run(count,
             [&](const Readback& readback)
             {
                 const auto& ids = readback.floats(kernel.output);
                 auto correct = 0;

                 for (auto i = 0; i < count; ++i)
                     if (ids[i] == (float) (i % width))
                         ++correct;

                 check(correct == count, readback.name());
             });
}

int countWritten(const Vector<float>& values)
{
    auto written = 0;

    for (auto value: values)
        if (value != untouched)
            ++written;

    return written;
}
} // namespace

// Each group sums the 256 elements it owns, which is four times what the stock
// group would have reached.
auto tWideGroupSums = test("ThreadGroupSize/aWideGroupSumsItsOwnRun") = []
{
    constexpr auto groups = 3;
    constexpr auto count = wideGroup * groups;

    auto values = Vector<float> {};
    values.resize(count);

    for (auto i = 0; i < count; ++i)
        values[i] = (float) (i % 7 + 1);

    auto kernel = WideSumKernel {};

    CrossCheck {kernel}
        .input(kernel.input, values)
        .output(kernel.output, groups, untouched)
        .agreeing()
        .run(count,
             [&](const Readback& readback)
             {
                 const auto& result = readback.floats(kernel.output);

                 for (auto group = 0; group < groups; ++group)
                 {
                     auto expected = 0.f;

                     for (auto i = group * wideGroup; i < (group + 1) * wideGroup;
                          ++i)
                         expected += values[i];

                     check(result[group] == expected, readback.name());
                 }
             });
};

// A 16x16 group, checked by the one arrangement of its tile that only a group
// of that shape produces.
auto tTileGroupTransposes =
    test("ThreadGroupSize/aTiledGroupTransposesItsBlock") = []
{
    constexpr auto side = tile * 2;
    constexpr auto cells = side * side;

    auto values = Vector<float> {};
    values.resize(cells);

    for (auto y = 0; y < side; ++y)
        for (auto x = 0; x < side; ++x)
            values[y * side + x] = (float) (y * side + x);

    auto kernel = TileTransposeKernel {};

    CrossCheck {kernel}
        .input(kernel.input, values)
        .output(kernel.output, cells, untouched)
        .agreeing()
        .run(side,
             side,
             [&](const Readback& readback)
             {
                 const auto& result = readback.floats(kernel.output);
                 auto correct = 0;

                 for (auto y = 0; y < side; ++y)
                     for (auto x = 0; x < side; ++x)
                         if (result[y * side + x] == values[x * side + y])
                             ++correct;

                 check(correct == cells, readback.name());

                 // And genuinely transposed rather than copied, which the
                 // diagonal alone would not have told apart.
                 check(result[1] != values[1], readback.name());
             });
};

// The width a lane counts to is its own kernel's, and a kernel that named no
// shape still counts to the stock one.
auto tLocalIdRunsToTheGroupWidth =
    test("ThreadGroupSize/aLaneCountsToItsOwnGroupWidth") = []
{
    constexpr auto count = wideGroup * 2;

    auto stock = LocalIdKernel {};
    expectLocalIdsWrap(stock, count, ComputeProgram::groupWidth);

    auto wide = WideLocalIdKernel {};
    expectLocalIdsWrap(wide, count, wideGroup);
};

// The 2D default, unchanged by any of this: 8x8.
auto tStockGridGroupIsEightSquared =
    test("ThreadGroupSize/theStockGridGroupIsEightSquared") = []
{
    constexpr auto side = 32;
    constexpr auto cells = side * side;
    constexpr auto stock = ComputeProgram::groupSize2D;

    auto kernel = LocalPositionKernel {};

    CrossCheck {kernel}
        .output(kernel.output, cells, untouched)
        .agreeing()
        .run(side,
             side,
             [&](const Readback& readback)
             {
                 const auto& result = readback.floats(kernel.output);
                 auto correct = 0;

                 for (auto y = 0; y < side; ++y)
                     for (auto x = 0; x < side; ++x)
                         if (result[y * side + x]
                             == (float) (x % stock) + (float) (y % stock) * 1000.f)
                             ++correct;

                 check(correct == cells, readback.name());
             });
};

// The indirect path takes the group from the pipeline it is dispatching, so a
// grid of n groups runs n * 256 threads and nothing else produces that number.
auto tIndirectDispatchUsesTheProgramsGroup =
    test("ThreadGroupSize/anIndirectDispatchRunsTheProgramsGroup") = []
{
    constexpr auto capacity = 1024;
    constexpr auto marked = 300;
    constexpr auto groups = (marked + wideGroup - 1) / wideGroup;

    std::uint32_t initial[] = {0u, 1u, 1u, (std::uint32_t) marked};

    auto onCpu = filled(capacity, untouched);

    {
        auto words = Vector<std::uint32_t> {};

        for (auto word: initial)
            words.add(word);

        auto prepare = PrepareWideKernel {};
        auto prepareBindings = CpuCompute::Bindings {};
        check(prepareBindings.set(prepare.arguments, words));
        dispatchOnCpu(prepare, prepareBindings, 1);

        auto consume = WideConsumeKernel {};
        auto consumeBindings = CpuCompute::Bindings {};
        check(consumeBindings.set(consume.output, onCpu));
        dispatchIndirectOnCpu(consume, consumeBindings, words, capacity);

        check(countWritten(onCpu) == groups * wideGroup, "cpu");
    }

    auto& device = Device::shared();

    if (!device.isValid())
        return;

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

    auto onGpu = readAll(output, capacity);

    check(countWritten(onGpu) == groups * wideGroup, "gpu");
    expectAgreement(onCpu, onGpu);
};
