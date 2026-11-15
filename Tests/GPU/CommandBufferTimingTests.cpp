#include "Common.h"

#include <string>

// CommandBuffer::timings() - what the GPU spent on the labelled passes of an
// off-screen command buffer, which has no frame to report through.
//
// The assertions are shaped like FrameTimingTests': the bookkeeping is exact -
// which labels come back, in what order, how many - and the durations are a
// measurement, held to bounds and to one ordering between passes far apart in
// cost that have to run in turn.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace CommandBufferTimingTests
{
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

// The bounds a measurement can be held to. The buffer's own total needs no
// counters and is reported everywhere; the breakdown needs them, and is asked
// about only once the timings have been resolved, because resolving is where
// an adapter that only claimed to have counters is found out and support
// retired. The Parallels virtual GPU is one: it answers with a heap of zeroes,
// and reading it is what turns supportsPassTimings() false - before that it
// still says true, and a column of zeroes would fail the per-pass bound below.
void checkBounded(const CommandBuffer& commands, const FrameTimings& timings)
{
    check(timings.milliseconds >= 0.0);
    check(timings.milliseconds < 1000.0);

    if (!commands.supportsPassTimings())
        return;

    auto total = 0.0;

    for (const auto& pass: timings.passes)
    {
        // Zero is what a sample the GPU never wrote reads as, so this is the
        // case that fails if the sample indices are swapped or never reach the
        // pass descriptor.
        check(pass.spanMilliseconds > 0.0);
        check(pass.spanMilliseconds <= timings.milliseconds + 0.05);

        check(pass.milliseconds >= 0.0);
        check(pass.milliseconds <= pass.spanMilliseconds + 1e-6);

        total += pass.milliseconds;
    }

    check(total <= timings.milliseconds + 0.05);
}

constexpr auto heavyIterations = 4096;

// The same ramp, made expensive by a loop that feeds the value it writes, so
// the compiler cannot drop it.
struct HeavyRampKernel final : ComputeProgram
{
    HeavyRampKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto total = var(0.f);
        auto step = var(0);

        loop(step < heavyIterations,
             [&]
             {
                 total += sin(toFloat(i + toUInt(step)));
                 step += 1;
             });

        write(output, i, toFloat(i) * 0.5f + total * 1e-6f);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};
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
    checkBounded(commands, timings);

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
    checkBounded(commands, timings);

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
    checkBounded(commands, timings);

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
// The figure that adds up, on a buffer of passes that all write one buffer and
// so run in turn: the shares cover the buffer, and the heavy pass in the middle
// is charged for itself rather than spread over the light ones around it. Asked
// of the best of a few buffers, as FrameTiming/sharesSumToTheFrame is and for
// its reason: another process's work can land inside any one pass.
auto tSharesSumToTheBuffer = test("CommandBufferTiming/sharesSumToTheBuffer") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto passCount = 5;
    constexpr auto heavyPass = 2;
    constexpr auto attempts = 4;

    auto output = makeOutput();

    auto light = RampKernel {};
    light.output = output;
    light.prepare();

    auto heavy = HeavyRampKernel {};
    heavy.output = output;
    heavy.prepare();

    auto accounted = false;

    for (auto attempt = 0; attempt < attempts && !accounted; ++attempt)
    {
        auto commands = device.makeCommandBuffer();

        for (auto index = 0; index < passCount; ++index)
        {
            auto pass = commands.beginCompute("pass" + std::to_string(index));

            if (index == heavyPass)
                pass.dispatch(heavy, elementCount);
            else
                pass.dispatch(light, elementCount);
        }

        commands.commit();

        const auto& timings = commands.timings();
        checkBounded(commands, timings);

        if (!commands.supportsPassTimings())
            return;

        check(timings.passes.size() == passCount);

        if (timings.passes.size() != passCount)
            return;

        auto total = 0.0;

        for (const auto& pass: timings.passes)
            total += pass.milliseconds;

        const auto heavyShare = timings.passes[heavyPass].milliseconds;

        accounted = total >= timings.milliseconds * 0.75 - 0.05;

        for (auto index = 0; index < passCount; ++index)
            if (index != heavyPass
                && timings.passes[index].milliseconds >= heavyShare * 0.5)
                accounted = false;
    }

    check(accounted);
};
} // namespace CommandBufferTimingTests
