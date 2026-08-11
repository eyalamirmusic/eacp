#include <eacp/GPU/GPU.h>

#include <eacp/Core/Utils/Containers.h>

#include <chrono>
#include <cstdio>

using namespace eacp;
using namespace GPU;

namespace
{
// Large enough that the dispatch takes a few milliseconds; work that finishes
// first has nothing to overlap with.
constexpr int elementCount = 1 << 23;
constexpr auto elementBytes = sizeof(float) * (std::size_t) elementCount;

// Sized so the CPU side outlasts the dispatch in an optimised build too.
constexpr int cpuWorkIterations = 40'000'000;

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

// Every caller keeps the result and report() prints it, so the optimiser cannot
// delete the work being measured.
double doCpuWork()
{
    auto total = 0.0;

    for (auto i = 1; i < cpuWorkIterations; ++i)
        total += 1.0 / (double) i;

    return total;
}

// A CommandBuffer owns a live recording and neither copies nor moves, so it is
// passed in; returning closes the encoder, which the commit needs.
void recordDispatch(CommandBuffer& commands, MixKernel& kernel)
{
    auto pass = commands.beginCompute();
    pass.dispatch(kernel, elementCount);
}

struct Timing
{
    double toReturn = 0.0;
    double total = 0.0;
    double cpuSum = 0.0;
};

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

    // Pumps the event loop until the completion callback has arrived.
    finished.waitFor(Time::MS {5000});
    timing.total = millisSince(start);

    return timing;
}

int countMismatches(const Buffer& left, const Buffer& right)
{
    // Parenthesised: under braces Vector's initializer_list constructor would
    // win and make a one-element vector the read overruns.
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
    auto warmupSum = doCpuWork();

    auto blocking = runBlocking(device, kernel, blockingOutput);
    auto async = runNonBlocking(device, kernel, asyncOutput);

    report(blocking, async, countMismatches(blockingOutput, asyncOutput), warmupSum);
}
} // namespace

int main()
{
    return Apps::run(runAsyncCompute);
}
