#include "Common.h"

// Where a kernel's writes actually happen.
//
// A store used to be a *root* of the compute graph rather than a statement in
// it - collected into a list and emitted after the body, whatever block the
// write() call had been made in. So a store inside an ifThen ran regardless of
// the condition, and one inside a loop ran once after it, on the counter's
// final value. Both compiled, both produced plausible output, and neither said
// anything.
//
// The shipped kernels never hit it because all of them write at the top level,
// which is exactly why it survived. These are the two shapes that catch it, and
// they check the values rather than the source: what matters is not that the
// emitted text has a brace in the right place but that the elements the kernel
// was told to leave alone still hold what they held.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto elementCount = 64;
constexpr auto perThread = 4;

// The value every element starts at, and which no kernel here ever writes. An
// element still holding it was not written; one holding anything else was.
constexpr auto untouched = -1.f;

// Writes only where the thread index is even. The odd elements are the
// evidence: a store hoisted out of the ifThen writes all of them.
struct GuardedStoreKernel final : ComputeProgram
{
    GuardedStoreKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        ifThen(id % 2u == 0u, [&] { write(output, id, toFloat(id)); });
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

// Each thread fills its own run of consecutive elements from inside a loop. A
// store emitted after the loop writes one element instead of perThread, at the
// index the counter finished on - so both how many were written and which ones
// are wrong, and neither is a crash.
struct LoopStoreKernel final : ComputeProgram
{
    LoopStoreKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto base = id * (unsigned) perThread;
        auto i = var(0);

        loop(i < perThread,
             [&]
             {
                 auto at = base + toUInt(i);
                 write(output, at, toFloat(at) + 100.f);
                 i += 1;
             });
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

Buffer makeFilled(int elements, float value)
{
    auto initial = Vector<float> {};
    initial.assign(elements, value);

    return Buffer {Device::shared(),
                   initial.data(),
                   sizeof(float) * (std::size_t) elements,
                   BufferUsage::Storage};
}

Vector<float> readBack(const Buffer& buffer, int elements)
{
    auto values = Vector<float> {};
    values.resize(elements);
    buffer.read(values.data(), sizeof(float) * (std::size_t) elements);
    return values;
}

template <typename Kernel>
Vector<float>
    runOver(Kernel& kernel, const Buffer& output, int threads, int elements)
{
    kernel.output = output;
    kernel.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();
    return readBack(output, elements);
}
} // namespace

// The odd elements must come back untouched. Before stores were statements they
// came back written, because the ifThen emitted an empty body and the store
// followed it unconditionally.
auto tGuardedStoreRespectsTheGuard = test("StorePlacement/aStoreObeysItsIfThen") = []
{
    if (!Device::shared().isValid())
        return;

    auto output = makeFilled(elementCount, untouched);
    auto kernel = GuardedStoreKernel {};
    auto values = runOver(kernel, output, elementCount, elementCount);

    auto written = 0;
    auto skipped = 0;

    for (auto i = 0; i < elementCount; ++i)
    {
        if (i % 2 == 0)
        {
            if (values[i] == (float) i)
                ++written;

            continue;
        }

        if (values[i] == untouched)
            ++skipped;
    }

    check(written == elementCount / 2);
    check(skipped == elementCount / 2);
};

// Every element of every thread's run, written from inside the loop body. A
// store emitted after the loop leaves perThread - 1 of every run untouched.
auto tLoopStoreRunsEveryIteration =
    test("StorePlacement/aStoreInsideALoopRepeats") = []
{
    if (!Device::shared().isValid())
        return;

    auto threads = elementCount / perThread;
    auto output = makeFilled(elementCount, untouched);
    auto kernel = LoopStoreKernel {};
    auto values = runOver(kernel, output, threads, elementCount);

    auto correct = 0;

    for (auto i = 0; i < elementCount; ++i)
        if (values[i] == (float) i + 100.f)
            ++correct;

    check(correct == elementCount);
};
