#include "Common.h"

// The one thing a reduction cannot rule out.
//
// FrameCompute/sharedMemoryGroupSums folds a tile in half per barrier, which
// says the memory is shared and the barrier orders it - but a tree reduction
// reads slots this thread's own half of the tile wrote, so a lane that read
// only itself would still fold to something. What is left to check is the flat
// case: every thread reads the lane **opposite** its own, so no thread reads
// anything it wrote. On per-thread scratch that comes back as the identity
// rather than as a reversal, which no amount of arithmetic can disguise.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto groupSize = ComputePass::threadGroupWidth;
constexpr auto groups = 5;
constexpr auto threadCount = groupSize * groups;

struct ExchangeKernel final : ComputeProgram
{
    ExchangeKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto lane = localId();
        auto scratch = shared<Float>(groupSize);

        write(scratch, lane, toFloat(id));
        barrier();

        // A kernel that barriers has no bounds guard, so the store is what
        // holds the tail of the last group inside the buffer. The dispatch is a
        // whole number of groups here, but writing it any other way would be
        // relying on that.
        auto opposite = scratch[(unsigned) (groupSize - 1) - lane];
        ifThen(id < gridCount(), [&] { write(output, id, opposite); });
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

// Thread `lane` of a group comes back holding what thread `groupSize - 1 - lane`
// wrote.
auto tExchangeCrossesLanes = test("SharedMemory/everyThreadReadsAnotherLane") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output =
        device.makeBuffer(sizeof(float) * threadCount, BufferUsage::Storage);

    auto kernel = ExchangeKernel {};
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

    float values[threadCount] = {};
    output.read(values, sizeof(values));

    auto correct = 0;

    for (auto i = 0; i < threadCount; ++i)
    {
        auto base = (i / groupSize) * groupSize;
        auto expected = (float) (base + groupSize - 1 - (i % groupSize));

        if (values[i] == expected)
            ++correct;
    }

    check(correct == threadCount);

    // And it is genuinely a reversal rather than the identity, which is what a
    // kernel with unshared scratch would have produced.
    check(values[0] != 0.f);
};

namespace
{
// A kilobyte of tile, which every device eacp runs on has room for, beside a
// reduction whose scratch the emitter adds behind it.
constexpr auto tileElements = 256;

struct BudgetedKernel final : ComputeProgram
{
    BudgetedKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto lane = localId();
        auto tile = shared<Float>(tileElements);

        write(tile, lane, input[id]);
        barrier();

        auto total = groupSum(tile[lane]);
        ifThen(id < gridCount(), [&] { write(output, id, total); });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// A tile of three-vectors, which is the one element type whose bytes and whose
// stride differ: MSL packs a float3 to twelve and an std430 block and a DXBC
// groupshared array give it sixteen.
struct WideElementKernel final : ComputeProgram
{
    WideElementKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto lane = localId();
        auto tile = shared<Float3>(tileElements);

        write(tile, lane, float3(input[id], input[id], input[id]));
        barrier();

        ifThen(id < gridCount(), [&] { write(output, id, tile[lane].x()); });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// Past every budget any of the three backends reports: Vulkan's floor is 16 KB,
// the two with a fixed number give 32 KB, and no Metal device reaches a
// megabyte.
struct OverBudgetKernel final : ComputeProgram
{
    OverBudgetKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto lane = localId();
        auto tile = shared<Float>(1024 * 1024);

        write(tile, lane, input[id]);
        barrier();

        ifThen(id < gridCount(), [&] { write(output, id, tile[lane]); });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};
} // namespace

// What the device allows, which is the number a kernel author has had to carry
// in a comment until now.
auto tThreadgroupBudgetIsReported =
    test("SharedMemory/theDeviceReportsItsThreadgroupBudget") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    // Vulkan's spec floor, and so what a kernel may assume on any of the three.
    check(device.maxThreadgroupMemory() >= 16 * 1024);
};

// What a kernel spends: its own arrays plus the scratch the emitter adds for a
// reduction, counted in the same bytes the budget is in.
auto tKernelReportsWhatItDeclares =
    test("SharedMemory/aKernelReportsWhatItDeclares") = []
{
    auto& device = Device::shared();

    auto budgeted = BudgetedKernel {};

    auto tile = tileElements * (int) sizeof(float);
    auto scratch = ComputePass::threadGroupWidth * (int) sizeof(float);

    check(budgeted.threadgroupMemoryBytes() == tile + scratch);

    // A vector element is counted at the stride the backends that pad give it,
    // not at the twelve bytes MSL packs a three-vector to: the budget is what a
    // kernel is written against, and a kernel is written once.
    auto wide = WideElementKernel {};

    check(wide.threadgroupMemoryBytes() == tileElements * 16);

    if (!device.isValid())
        return;

    check(budgeted.fitsThreadgroupMemory(device));

    // And the overspend is answered before the backend is asked to make a
    // pipeline: prepare() names the two numbers in the log and then builds
    // anyway. What comes back is the backend's business and differs by
    // backend - Metal refuses the pipeline, lavapipe hands back one that would
    // misbehave at dispatch - so the log line, not the pipeline's validity, is
    // what this asserts by exercising it.
    auto over = OverBudgetKernel {};

    check(over.threadgroupMemoryBytes() > device.maxThreadgroupMemory());
    check(!over.fitsThreadgroupMemory(device));

    over.prepare(device);
};
