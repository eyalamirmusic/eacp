#include "Common.h"

// The count is never read back: only its effect, a second kernel that ran as
// many times as the first decided it should.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto capacity = 512;

// Deliberately not a multiple of the threadgroup width, so the rounding up to
// whole groups is exercised and the guard has something to stop.
constexpr auto marked = 137;

// Elements 0..2 are the per-axis group counts an indirect dispatch reads; 1 and
// 2 are one because this is a 1D grid. Element 3 sits past the arguments and
// holds the unrounded item count.
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

// Separate from the counting kernel because it has to run after every thread of
// it has finished, and only the end of a dispatch says so.
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

// Unguarded on purpose: what has to be observable is how many threads ran, and
// a guarded kernel writes the same output however large the grid was. A guarded
// first cut passed against a dispatch that ignored the argument buffer.
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

// Guarded against the exact count, the way a real stage is written.
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

// Not zero: a thread that ran and a thread that never ran must be
// distinguishable.
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

// Every count stays on the GPU: only the output buffer crosses back.
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

// 137 items ask for ceil(137/64) = 3 groups, so 192 threads run - neither the
// count nor the capacity the CPU handed the dispatch as a guard.
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

auto tIndirectZeroGridRunsNothing =
    test("IndirectDispatch/anEmptyCountRunsNoThreads") = []
{
    if (!Device::shared().isValid())
        return;

    auto consume = ConsumeKernel {};
    auto values = runPipeline(makeCandidates(0), consume);

    check(countWritten(values) == 0);
};

// The grid is rounded up to whole groups: 192 threads run, 137 of them write.
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
