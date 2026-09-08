#include "Common.h"

// CommandBuffer::timings() - what the GPU spent on the labelled passes of an
// off-screen command buffer, which has no frame to report through.
//
// The assertions are shaped like FrameTimingTests': the bookkeeping is exact -
// which labels come back, in what order, how many - and the durations are a
// measurement, so the only honest statements about them are bounds.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto elementCount = 1 << 14;

struct RampKernel final : ComputeProgram
{
    RampKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, toFloat(i) * 0.5f);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

Buffer makeOutput()
{
    return Device::shared().makeBuffer((int) sizeof(float) * elementCount,
                                       BufferUsage::Storage);
}

void checkBounded(const FrameTimings& timings)
{
    check(timings.milliseconds >= 0.0);
    check(timings.milliseconds < 1000.0);

    for (const auto& pass: timings.passes)
    {
        // Zero is what a sample the GPU never wrote reads as, so this is the
        // case that fails if the two sample indices are swapped or never reach
        // the pass descriptor.
        check(pass.milliseconds > 0.0);
        check(pass.milliseconds <= timings.milliseconds + 0.05);
    }
}
} // namespace

// The wiring: the labels the passes were given come back, all of them, in the
// order they were encoded, with the buffer's own total covering each of them.
auto tLabelledPassesComeBack =
    test("CommandBufferTiming/labelledPassesComeBack") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto first = makeOutput();
    auto second = makeOutput();

    auto kernel = RampKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        kernel.output = first;
        auto pass = commands.beginCompute("first");
        pass.dispatch(kernel, elementCount);
    }

    {
        kernel.output = second;
        auto pass = commands.beginCompute("second");
        pass.dispatch(kernel, elementCount);
    }

    commands.commit();

    const auto& timings = commands.timings();
    checkBounded(timings);

    if (!commands.supportsPassTimings())
        return;

    check(timings.passes.size() == 2);
    check(timings.passes[0].label == "first");
    check(timings.passes[1].label == "second");
};

// The other half of the contract, and the reason the label is the switch: a
// pass that does not ask to be timed is not timed, and does not appear.
auto tUnlabelledPassIsNotTimed =
    test("CommandBufferTiming/unlabelledPassIsNotTimed") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeOutput();

    auto kernel = RampKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, elementCount);
    }

    {
        auto pass = commands.beginCompute("timed");
        pass.dispatch(kernel, elementCount);
    }

    commands.commit();

    const auto& timings = commands.timings();
    checkBounded(timings);

    if (!commands.supportsPassTimings())
        return;

    check(timings.passes.size() == 1);
    check(timings.passes[0].label == "timed");
};

// The submission that does not wait reports the same way: the numbers are the
// GPU's, so they exist once it has finished rather than once commit() returned.
auto tAsyncCommitReportsToo = test("CommandBufferTiming/asyncCommitReportsToo") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeOutput();

    auto kernel = RampKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute("async");
        pass.dispatch(kernel, elementCount);
    }

    auto finished = commands.commitAsync();

    // Resolving needs the event loop to turn, which is what waitFor pumps.
    finished.waitFor(Time::MS {5000});
    check(finished.isResolved());

    const auto& timings = commands.timings();
    checkBounded(timings);

    if (!commands.supportsPassTimings())
        return;

    check(timings.passes.size() == 1);
    check(timings.passes[0].label == "async");
};

// A command buffer nobody asked to time costs nothing and says nothing, which
// is what keeps the timestamp resources off every off-screen dispatch.
auto tUntimedBufferReportsNothing =
    test("CommandBufferTiming/untimedBufferReportsNothing") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeOutput();

    auto kernel = RampKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, elementCount);
    }

    commands.commit();

    check(commands.timings().passes.size() == 0);
    check(commands.timings().milliseconds == 0.0);
};
