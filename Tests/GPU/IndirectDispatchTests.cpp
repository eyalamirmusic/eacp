#include "Common.h"

// A dispatch whose size the GPU decided.
//
// The point is not that a grid can be read out of a buffer - it is that the
// number never reaches the CPU. A stage sized by what the stage before it found
// would otherwise need a readback between two passes that were going to be
// adjacent, and a readback is a round trip through the host.
//
// So the test never reads the count. It reads only the *effect*: a second kernel
// that ran exactly as many times as the first kernel decided it should. The
// count is derived from buffer contents the test writes, so the answer is known
// here without ever having been transferred back.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto capacity = 512;

// How many of the candidates are marked. Deliberately not a multiple of the
// threadgroup width, so the rounding up to whole groups is exercised and the
// guard has something to stop.
constexpr auto marked = 137;

// Counts the marked candidates and turns that into a threadgroup count, which
// is what an indirect dispatch reads. Element 0 is the count of groups; 1 and 2
// are the other two axes, and are set to one because this is a 1D grid.
//
// Element 3 is past the arguments and holds the exact item count - the
// arguments themselves are rounded up to whole groups, so the kernel that runs
// next needs the unrounded number to know where to stop.
struct CountKernel final : ComputeProgram
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

// One thread, turning the count into the grid. Separate from the counting
// kernel because it has to run after every thread of it has finished, and the
// only thing that orders threads of one dispatch against each other is the end
// of the dispatch.
struct PrepareKernel final : ComputeProgram
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

// The indirectly dispatched stage, writing at its own index and guarding
// nothing. Unguarded on purpose: what has to be observable is **how many
// threads ran**, and a kernel that guards itself writes the same output however
// large the grid was. The first cut of this test did guard, and it passed
// against a dispatch that ignored the argument buffer outright - which is how
// this one came to be written the other way round.
//
// So the mark left behind is the grid itself. A dispatch of n groups writes
// exactly n * threadGroupWidth elements, and nothing else produces that number.
struct ConsumeKernel final : ComputeProgram
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

// The same stage as a pipeline would really write it: guarded against the exact
// count, so the tail of the last group does nothing. The one above is the
// instrument; this is the pattern.
struct GuardedConsumeKernel final : ComputeProgram
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

// What an element of the output holds if nothing wrote it. Not zero, because a
// kernel that ran and wrote zero and a kernel that never ran have to be
// distinguishable - counting how many threads ran is the whole measurement.
constexpr auto untouched = -1.f;

Buffer makeCandidates(int howMany)
{
    auto values = Vector<float> {};
    values.assign(capacity, 0.f);

    for (auto i = 0; i < howMany; ++i)
        values[i] = 1.f;

    return Buffer {Device::shared(),
                   values.data(),
                   sizeof(float) * (std::size_t) capacity,
                   BufferUsage::Storage};
}

// Four uints: the three the dispatch reads, and the exact count after them.
Buffer makeArguments()
{
    std::uint32_t initial[] = {0u, 1u, 1u, 0u};

    return Buffer {Device::shared(), initial, sizeof(initial), BufferUsage::Storage};
}

// The three stages, with whatever consumer is being observed dispatched
// indirectly at the end. Every count stays on the GPU: the only thing that
// crosses back is the output buffer.
template <typename Consumer>
Vector<float> runPipeline(const Buffer& candidates, Consumer& consume)
{
    auto arguments = makeArguments();

    auto blank = Vector<float> {};
    blank.assign(capacity, untouched);

    auto output = Buffer {Device::shared(),
                          blank.data(),
                          sizeof(float) * (std::size_t) capacity,
                          BufferUsage::Storage};

    auto counter = CountKernel {};
    counter.candidates = candidates;
    counter.arguments = arguments;
    counter.prepare();

    auto prepare = PrepareKernel {};
    prepare.arguments = arguments;
    prepare.prepare();

    consume.output = output;
    consume.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(counter, capacity);
    }

    // Its own pass, because it has to see every thread of the counting kernel
    // finished, and the end of a dispatch is the only thing that says so.
    {
        auto pass = commands.beginCompute();
        pass.dispatch(prepare, 1);
    }

    {
        auto pass = commands.beginCompute();
        pass.dispatchIndirect(consume, arguments, capacity);
    }

    commands.commit();

    auto values = Vector<float> {};
    values.resize(capacity);
    output.read(values.data(), sizeof(float) * (std::size_t) capacity);
    return values;
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

// How many threads ran, read off what they wrote. The counting kernel found 137
// items and asked for ceil(137/64) = 3 groups, so exactly 192 threads ran - a
// number that is neither the count, nor the capacity, nor anything the CPU
// passed in.
//
// That last part is the test. `capacity` is what the CPU handed the dispatch as
// a guard, and a dispatch that ignored the argument buffer would have run all
// 512; a grid of 192 could only have come from the buffer.
auto tIndirectGridComesFromTheGpu =
    test("IndirectDispatch/theGridComesFromAKernel") = []
{
    if (!Device::shared().isValid())
        return;

    auto consume = ConsumeKernel {};
    auto values = runPipeline(makeCandidates(marked), consume);

    auto groups =
        (marked + ComputePass::threadGroupWidth - 1) / ComputePass::threadGroupWidth;

    check(countWritten(values) == groups * ComputePass::threadGroupWidth);
    check(countWritten(values) != capacity);
    check(countWritten(values) != marked);
};

// A grid of zero. The counting kernel finds nothing, so the prepared group count
// is zero and the consuming kernel must not run at all - the case a pipeline
// hits whenever the thing it was looking for is absent, and the one where "no
// groups" has to mean no work rather than all of it.
auto tIndirectZeroGridRunsNothing =
    test("IndirectDispatch/anEmptyCountRunsNoThreads") = []
{
    if (!Device::shared().isValid())
        return;

    auto consume = ConsumeKernel {};
    auto values = runPipeline(makeCandidates(0), consume);

    check(countWritten(values) == 0);
};

// And the pattern a real stage uses: the grid is rounded up to whole groups, so
// the consumer guards against the exact count and the tail of the last group
// does nothing. 192 threads run and 137 of them write.
auto tGuardedConsumerStopsAtTheCount =
    test("IndirectDispatch/aGuardedStageStopsAtTheExactCount") = []
{
    if (!Device::shared().isValid())
        return;

    auto arguments = makeArguments();

    auto blank = Vector<float> {};
    blank.assign(capacity, untouched);

    auto output = Buffer {Device::shared(),
                          blank.data(),
                          sizeof(float) * (std::size_t) capacity,
                          BufferUsage::Storage};

    auto candidates = makeCandidates(marked);

    auto counter = CountKernel {};
    counter.candidates = candidates;
    counter.arguments = arguments;
    counter.prepare();

    auto prepare = PrepareKernel {};
    prepare.arguments = arguments;
    prepare.prepare();

    auto consume = GuardedConsumeKernel {};
    consume.arguments = arguments;
    consume.output = output;
    consume.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(counter, capacity);
    }

    {
        auto pass = commands.beginCompute();
        pass.dispatch(prepare, 1);
    }

    {
        auto pass = commands.beginCompute();
        pass.dispatchIndirect(consume, arguments, capacity);
    }

    commands.commit();

    auto values = Vector<float> {};
    values.resize(capacity);
    output.read(values.data(), sizeof(float) * (std::size_t) capacity);

    check(countWritten(values) == marked);
};
