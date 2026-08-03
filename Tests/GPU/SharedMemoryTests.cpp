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
