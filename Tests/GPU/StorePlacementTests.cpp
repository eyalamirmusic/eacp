#include "CpuCrossCheck.h"

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
using namespace eacp::GPU::CrossChecks;

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

// The kernel over an output of elementCount untouched elements, on the CPU and
// then the GPU; verify is handed what each left behind and the backend's name.
template <typename Kernel, typename Verify>
void runOver(Kernel& kernel, int threads, Verify verify)
{
    CrossCheck {kernel}
        .output(kernel.output, elementCount, untouched)
        .run(threads,
             [&](const Readback& readback)
             { verify(readback.floats(kernel.output), readback.name()); });
}
} // namespace

// The odd elements must come back untouched. Before stores were statements they
// came back written, because the ifThen emitted an empty body and the store
// followed it unconditionally.
auto tGuardedStoreRespectsTheGuard = test("StorePlacement/aStoreObeysItsIfThen") = []
{
    auto kernel = GuardedStoreKernel {};

    runOver(kernel,
            elementCount,
            [](const Vector<float>& values, const char* name)
            {
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

                check(written == elementCount / 2, name);
                check(skipped == elementCount / 2, name);
            });
};

// Every element of every thread's run, written from inside the loop body. A
// store emitted after the loop leaves perThread - 1 of every run untouched.
auto tLoopStoreRunsEveryIteration =
    test("StorePlacement/aStoreInsideALoopRepeats") = []
{
    auto kernel = LoopStoreKernel {};

    runOver(kernel,
            elementCount / perThread,
            [](const Vector<float>& values, const char* name)
            {
                auto correct = 0;

                for (auto i = 0; i < elementCount; ++i)
                    if (values[i] == (float) i + 100.f)
                        ++correct;

                check(correct == elementCount, name);
            });
};
