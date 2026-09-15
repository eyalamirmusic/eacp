#include "Common.h"

#include <optional>
#include <string>

// CommandBuffer::submit/wait/isComplete and the read scoped to one submission.
//
// What is being pinned is the scope rather than the speed: a wait that reached
// past this command buffer would still give the right numbers, so every case
// here that could pass by accident is paired with a second command buffer whose
// work is still running. The last of them is the shape a step-by-step loop
// takes - record and submit step k+1, then wait for step k and read it - and it
// is the one that fails if wait() ever waits for the newest submission again.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto floatBytes = (int) sizeof(float);

constexpr auto elementCount = 1 << 12;
constexpr auto stepCount = 8;

// Slow enough to still be running while another command buffer is waited on,
// and no larger than it has to be for that.
constexpr auto slowElements = 1 << 20;
constexpr auto slowIterations = 512;

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

// Step k adds one to what step k-1 left, so the chain is a dependency across
// command buffers and a slot read from the wrong submission is off by a whole
// number rather than by a rounding.
struct ChainKernel final : ComputeProgram
{
    ChainKernel() { compile(); }

    void define() override
    {
        auto at = step + threadId();
        write(slots, at, slots[at - 1u] + 1.0f);
    }

    Uniform<OutputBuffer> slots;
    Uniform<UInt> step;

    EACP_SHADER(slots, step)
};

struct SlowKernel final : ComputeProgram
{
    SlowKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto total = var(0.f);
        auto i = var(0);

        loop(i < slowIterations,
             [&]
             {
                 total += sin(toFloat(id + toUInt(i)));
                 i += 1;
             });

        write(output, id, total);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

Buffer makeOutput(int elements)
{
    return Device::shared().makeBuffer(floatBytes * elements, BufferUsage::Storage);
}

Buffer makeZeroed(int elements)
{
    auto zeros = Vector<float> {};
    zeros.assign(elements, 0.f);

    return Buffer {
        Device::shared(), zeros.data(), floatBytes * elements, BufferUsage::Storage};
}

// The other half of every case here: work handed to the same queue and left
// running, so a wait that is not scoped to one command buffer waits for this.
void dispatchSlowWork(CommandBuffer& commands, SlowKernel& kernel)
{
    auto pass = commands.beginCompute();
    pass.dispatch(kernel, slowElements);
}
} // namespace

// The trivial pairings, and the two answers a buffer that never reached the
// queue has to give: wait() returns, and isComplete() says no rather than yes.
auto tCommittedBufferIsComplete =
    test("CommandBufferWait/committedBufferIsComplete") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto never = device.makeCommandBuffer();
    never.wait();
    check(!never.isComplete());

    auto output = makeOutput(elementCount);

    auto kernel = RampKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();
    check(!commands.isComplete());

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, elementCount);
    }

    commands.commit();
    check(commands.isComplete());

    // Already finished, so this is the case that has to return rather than
    // block on a fence that has long since passed.
    commands.wait();
    check(commands.isComplete());
};

// submit() hands the work over and says nothing; wait() and the scoped read are
// the completion half. The slow buffer behind it is what makes the read a
// statement about scope: everything it returns was written before it.
auto tScopedReadSeesTheKernelsWrite =
    test("CommandBufferWait/scopedReadSeesTheKernelsWrite") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeOutput(elementCount);
    auto slowOutput = makeOutput(slowElements);

    auto kernel = RampKernel {};
    kernel.output = output;
    kernel.prepare();

    auto slow = SlowKernel {};
    slow.output = slowOutput;
    slow.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, elementCount);
    }

    commands.submit();

    auto trailing = device.makeCommandBuffer();
    dispatchSlowWork(trailing, slow);
    trailing.submit();

    commands.wait();
    check(commands.isComplete());

    auto values = Vector<float>(elementCount);
    commands.read(output, values.data(), floatBytes * elementCount);

    for (auto i = 0; i < elementCount; ++i)
        check(values[i] == (float) i * 0.5f);

    // The offset form, over the same finished work.
    auto one = 0.f;
    commands.read(output, &one, floatBytes, floatBytes * 7);
    check(one == 3.5f);

    trailing.wait();
};

// commitAsync() and wait() are a supported pairing: the Async settles on the
// message thread, and the wait is what a caller that is not turning it uses.
auto tAsyncCommitCanBeWaitedOn =
    test("CommandBufferWait/asyncCommitCanBeWaitedOn") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeOutput(elementCount);

    auto kernel = RampKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, elementCount);
    }

    auto finished = commands.commitAsync();

    commands.wait();
    check(commands.isComplete());

    auto values = Vector<float>(elementCount);
    commands.read(output, values.data(), floatBytes * elementCount);
    check(values[elementCount - 1] == (float) (elementCount - 1) * 0.5f);

    // And the promise still settles once the loop turns, the wait having taken
    // nothing away from it.
    finished.waitFor(Time::MS {5000});
    check(finished.isResolved());
};

// The shape a step-by-step loop takes: step k+1 is recorded and submitted
// before the CPU asks the GPU for step k, so the two overlap. Every step's slot
// is read back, and a read that saw an earlier submission's state - or one that
// waited for the whole queue and so could never have overlapped - shows up as a
// wrong number here rather than as a slower run.
auto tPipelinedStepsReadEverySlot =
    test("CommandBufferWait/pipelinedStepsReadEverySlot") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto chain = makeZeroed(stepCount);

    auto kernel = ChainKernel {};
    kernel.slots = chain;
    kernel.prepare();

    // In place, because a CommandBuffer is neither copyable nor movable, and
    // two is all a loop one step behind itself ever holds.
    std::optional<CommandBuffer> inFlight[2];

    auto results = Vector<float>(stepCount);

    const auto readStep = [&](int step)
    {
        inFlight[step % 2]->read(
            chain, &results[step], floatBytes, floatBytes * step);
    };

    for (auto step = 1; step < stepCount; ++step)
    {
        kernel.step = (unsigned) step;

        auto& commands = inFlight[step % 2].emplace(device);

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, 1);
        }

        commands.submit();

        if (step > 1)
            readStep(step - 1);
    }

    readStep(stepCount - 1);

    for (auto step = 1; step < stepCount; ++step)
        check(results[step] == (float) step);
};

// Two in flight, and the *first* is the one waited on. The second is long work
// submitted after it, so a read that came back with the right numbers only
// because it waited for the whole queue would have had to wait for that too.
auto tFirstIsReadableWhileSecondRuns =
    test("CommandBufferWait/firstIsReadableWhileSecondRuns") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeOutput(elementCount);
    auto slowOutput = makeOutput(slowElements);

    auto kernel = RampKernel {};
    kernel.output = output;
    kernel.prepare();

    auto slow = SlowKernel {};
    slow.output = slowOutput;
    slow.prepare();

    auto first = device.makeCommandBuffer();

    {
        auto pass = first.beginCompute();
        pass.dispatch(kernel, elementCount);
    }

    first.submit();

    auto second = device.makeCommandBuffer();
    dispatchSlowWork(second, slow);
    second.submit();

    auto values = Vector<float>(elementCount);
    first.read(output, values.data(), floatBytes * elementCount);

    for (auto i = 0; i < elementCount; ++i)
        check(values[i] == (float) i * 0.5f);

    // Left running is not left leaking: the second is drained before the
    // buffers it writes go out of scope.
    second.wait();
    check(second.isComplete());
};

// One labelled pass per kernel is what profiling a net looks like, and forty of
// them is well past the sixteen the pool used to hold.
auto tFortyLabelledPassesAreTimed =
    test("CommandBufferWait/fortyLabelledPassesAreTimed") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto labelledPasses = 40;

    auto output = makeOutput(elementCount);

    auto kernel = RampKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    for (auto pass = 0; pass < labelledPasses; ++pass)
    {
        auto timed = commands.beginCompute("pass" + std::to_string(pass));
        timed.dispatch(kernel, elementCount);
    }

    commands.commit();

    const auto& timings = commands.timings();

    if (!commands.supportsPassTimings())
        return;

    check(timings.passes.size() == labelledPasses);
    check(timings.passes[0].label == "pass0");
    check(timings.passes[labelledPasses - 1].label
          == "pass" + std::to_string(labelledPasses - 1));

    for (const auto& pass: timings.passes)
        check(pass.milliseconds > 0.0);
};
