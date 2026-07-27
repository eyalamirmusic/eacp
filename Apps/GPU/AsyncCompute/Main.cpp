#include <eacp/GPU/GPU.h>

#include <eacp/Core/Utils/Containers.h>

#include <chrono>
#include <cstdio>

using namespace eacp;
using namespace GPU;

// CommandBuffer::commitAsync: the same kernel, submitted two ways, timed.
//
// commit() blocks until the GPU is finished, so the CPU work that follows it
// starts only once the kernel has landed and the two costs add up. commitAsync()
// returns as soon as the work is submitted: the CPU work runs *while* the kernel
// does, and the wall clock is the longer of the two rather than their sum.
//
// Both runs produce the same buffer and the program checks that they do, because
// the interesting number is only interesting if the answer is still right.

namespace
{
// Large enough that the dispatch takes a good few milliseconds - there is
// nothing to overlap with work that finishes before the CPU gets going - and
// that a wrong answer shows up as more than one bad element.
constexpr int elementCount = 1 << 23;
constexpr auto elementBytes = sizeof(float) * (std::size_t) elementCount;

// Iterations of the stand-in CPU work below. Sized so the CPU side outlasts the
// dispatch in an optimised build as well as a debug one: when the GPU finishes
// first, the overlap is capped by the CPU work and the saving stops growing.
constexpr int cpuWorkIterations = 40'000'000;

// Deliberately arithmetic-heavy: a handful of transcendentals per element, so
// the GPU is busy for long enough to be worth not waiting for.
struct MixKernel final : ComputeProgram
{
    MixKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = toFloat(i) * scale;

        auto wave = sin(x) * cos(x * 1.7f) + sin(x * 0.3f) * 0.5f;
        auto shaped = wave / (abs(wave) + 1.0f);

        write(output, i, shaped * gain);
    }

    Uniform<OutputBuffer> output;
    Uniform<Float> scale;
    Uniform<Float> gain;

    EACP_SHADER(output, scale, gain)
};

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start)
{
    auto delta = Clock::now() - start;
    return std::chrono::duration<double, std::milli>(delta).count();
}

// Stands in for whatever else the frame has to get done - laying out a list,
// decoding audio, walking a scene. Every caller keeps the result and report()
// prints it, so the compiler cannot delete the work being measured.
double doCpuWork()
{
    auto total = 0.0;

    for (auto i = 1; i < cpuWorkIterations; ++i)
        total += 1.0 / (double) i;

    return total;
}

// Records the dispatch onto an already-created command buffer. A CommandBuffer
// is pinned to where it was made - it owns a live recording, so it neither
// copies nor moves - which is why this takes one rather than returning one.
//
// The pass has to end before the commit, which is what returning from here
// does: the encoder closes with the local.
void recordDispatch(CommandBuffer& commands, MixKernel& kernel)
{
    auto pass = commands.beginCompute();
    pass.dispatch(kernel, elementCount);
}

// What one run costs: how long the commit took to hand control back, and how
// long the whole run took including the CPU work beside it. The gap between
// them is the overlap - nothing for the blocking path, the dispatch for the
// other.
struct Timing
{
    double toReturn = 0.0;
    double total = 0.0;
    double cpuSum = 0.0;
};

// commit() waits for the GPU, so the CPU work below it cannot start until the
// kernel has landed and the two costs add up.
Timing runBlocking(Device& device, MixKernel& kernel, const Buffer& output)
{
    kernel.output = output;

    auto commands = device.makeCommandBuffer();
    recordDispatch(commands, kernel);

    auto timing = Timing {};
    auto start = Clock::now();

    commands.commit();
    timing.toReturn = millisSince(start);

    timing.cpuSum = doCpuWork();
    timing.total = millisSince(start);

    return timing;
}

// commitAsync() returns as soon as the work is submitted, so the CPU work runs
// while the kernel does and the wall clock is the longer of the two.
Timing runNonBlocking(Device& device, MixKernel& kernel, const Buffer& output)
{
    kernel.output = output;

    auto commands = device.makeCommandBuffer();
    recordDispatch(commands, kernel);

    auto timing = Timing {};
    auto start = Clock::now();

    auto finished = commands.commitAsync();
    timing.toReturn = millisSince(start);

    timing.cpuSum = doCpuWork();

    // Pumps the event loop until the completion callback has arrived. By now
    // the kernel has almost certainly finished, so this is where the overlap is
    // collected rather than where it is paid for.
    finished.waitFor(Time::MS {5000});
    timing.total = millisSince(start);

    return timing;
}

// How many elements the two runs disagree on. The interesting number is only
// interesting if the faster path computed the same thing.
int countMismatches(const Buffer& left, const Buffer& right)
{
    // Parenthesised: Vector's initializer_list constructor would win over the
    // sized one under braces, and make a one-element vector the read overruns.
    auto fromLeft = Vector<float>(elementCount);
    auto fromRight = Vector<float>(elementCount);

    left.read(fromLeft.data(), elementBytes);
    right.read(fromRight.data(), elementBytes);

    auto mismatches = 0;

    for (auto i = 0; i < elementCount; ++i)
        if (fromLeft[i] != fromRight[i])
            ++mismatches;

    return mismatches;
}

void report(const Timing& blocking,
            const Timing& async,
            int mismatches,
            double warmupSum)
{
    std::printf("  blocking commit()      %7.2f ms to return, %7.2f ms total\n",
                blocking.toReturn,
                blocking.total);
    std::printf("  non-blocking commitAsync() %7.2f ms to return, %7.2f ms total\n",
                async.toReturn,
                async.total);
    std::printf("\n  CPU work alone accounts for roughly %.2f ms of each.\n",
                blocking.total - blocking.toReturn);
    std::printf("  overlap saved %.2f ms.\n\n", blocking.total - async.total);

    // The sums are printed rather than dropped so the optimiser cannot delete
    // the CPU work the whole measurement is about. All three must agree.
    std::printf("  outputs %s (%d mismatches); CPU sums %.6f / %.6f / %.6f\n",
                mismatches == 0 ? "identical" : "DIFFER",
                mismatches,
                warmupSum,
                blocking.cpuSum,
                async.cpuSum);
}

void runAsyncCompute()
{
    auto& device = Device::shared();

    if (!device.isValid())
    {
        std::printf("AsyncCompute: no GPU device available; skipping.\n");
        return;
    }

    auto blockingOutput = device.makeBuffer(elementBytes);
    auto asyncOutput = device.makeBuffer(elementBytes);

    auto kernel = MixKernel {};
    kernel.scale = 0.0001f;
    kernel.gain = 0.9f;
    kernel.prepare();

    std::printf("AsyncCompute: %d elements, one dispatch each way.\n\n",
                elementCount);

    // Warm-up, so the two measured runs pay the same price for the CPU work.
    // Without it the first one absorbs the cold caches and the comparison
    // flatters whichever went second.
    auto warmupSum = doCpuWork();

    auto blocking = runBlocking(device, kernel, blockingOutput);
    auto async = runNonBlocking(device, kernel, asyncOutput);

    report(blocking, async, countMismatches(blockingOutput, asyncOutput), warmupSum);

    // read() after commitAsync is still correct on its own: it waits for the
    // same submission the Async resolves on. Waiting first, as runNonBlocking
    // does, is what turns that wait into nothing.
}
} // namespace

int main()
{
    return Apps::run(runAsyncCompute);
}
