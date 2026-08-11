#include "Common.h"

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

        // A kernel that barriers has no bounds guard, so this store is what
        // keeps the last group's tail inside the buffer.
        auto opposite = scratch[(unsigned) (groupSize - 1) - lane];
        ifThen(id < gridCount(), [&] { write(output, id, opposite); });
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

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

    // Unshared scratch would come back as the identity rather than a reversal.
    check(values[0] != 0.f);
};
