#include "Common.h"

// Regression: a store was a root of the compute graph rather than a statement,
// emitted after the body whatever block write() was called in - so a store
// inside an ifThen ran unconditionally and one inside a loop ran once, after it.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto elementCount = 64;
constexpr auto perThread = 4;

// No kernel here ever writes it, so an element still holding it was not written.
constexpr auto untouched = -1.f;

// The odd elements are the evidence: a hoisted store writes all of them.
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

// A store emitted after the loop writes one element instead of perThread, at
// the index the counter finished on.
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
