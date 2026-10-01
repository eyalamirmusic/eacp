#include <eacp/GPU/CpuCompute/CpuCompute.h>
#include <eacp/GPU/GPU.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

using namespace eacp;
using namespace GPU;

// One kernel, two backends: the same ComputeProgram dispatched through a
// ComputePass on the GPU and through CpuCompute::Executor on the CPU, over the
// same input, with the outputs compared and each path timed. Where no device
// came up, the CPU path runs alone - the kernel never needed one.

namespace
{
constexpr auto elementCount = 1 << 20;
constexpr auto elementBytes = (std::int64_t) sizeof(float) * elementCount;
constexpr auto repetitions = 20;
constexpr auto tolerance = 1e-5f;

// Blends two signals, soft-clips the blend with x / (1 + |x|) and applies a
// gain: a per-element stream kernel of the kind an audio plugin runs.
struct CrossfadeKernel final : ComputeProgram
{
    CrossfadeKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto mixed = mix(left[i], right[i], blend);
        auto shaped = mixed / (abs(mixed) + 1.0f);
        write(output, i, shaped * gain);
    }

    Uniform<InputBuffer> left;
    Uniform<InputBuffer> right;
    Uniform<OutputBuffer> output;
    Uniform<Float> blend;
    Uniform<Float> gain;
    EACP_SHADER(left, right, output, blend, gain)
};

using Clock = std::chrono::steady_clock;
using Floats = std::vector<float>;

double millisPerRun(const std::function<void()>& run)
{
    run();

    auto start = Clock::now();

    for (auto rep = 0; rep < repetitions; ++rep)
        run();

    auto total = std::chrono::duration<double, std::milli>(Clock::now() - start);
    return total.count() / repetitions;
}

Floats makeSignal(float frequency)
{
    auto values = Floats((std::size_t) elementCount);

    for (auto i = 0; i < elementCount; ++i)
        values[(std::size_t) i] = 1.5f * std::sin((float) i * frequency);

    return values;
}

double runOnGpu(CrossfadeKernel& kernel,
                const Floats& left,
                const Floats& right,
                Floats& result)
{
    auto& device = Device::shared();
    auto leftBuffer =
        device.makeBuffer(left.data(), elementBytes, BufferUsage::Storage);
    auto rightBuffer =
        device.makeBuffer(right.data(), elementBytes, BufferUsage::Storage);
    auto outputBuffer = device.makeBuffer(elementBytes);

    kernel.left = leftBuffer;
    kernel.right = rightBuffer;
    kernel.output = outputBuffer;
    kernel.prepare(device);

    auto millis = millisPerRun(
        [&]
        {
            auto commands = device.makeCommandBuffer();

            {
                auto pass = commands.beginCompute();
                pass.dispatch(kernel, elementCount);
            }

            commands.commit();
        });

    outputBuffer.read(result.data(), elementBytes);
    return millis;
}

// The threaded form: prepareDispatch copies the bindings and uniforms once,
// then each thread runs its own share of the groups in its own workspace.
// The threads are started inside the timed run, so their cost is included.
double runOnCpuThreads(CpuCompute::Executor& executor,
                       const CpuCompute::Bindings& bindings,
                       int threadCount)
{
    auto workspaces = std::vector<CpuCompute::Workspace> {};

    for (auto k = 0; k < threadCount; ++k)
        workspaces.emplace_back(executor.plan());

    return millisPerRun(
        [&]
        {
            auto prepared = executor.prepareDispatch(bindings, elementCount);
            auto each = (prepared.groupCount() + threadCount - 1) / threadCount;
            auto workers = std::vector<std::thread> {};

            for (auto k = 0; k < threadCount; ++k)
                workers.emplace_back(
                    [&, k]
                    {
                        executor.dispatchGroups(
                            prepared, k * each, each, workspaces[(std::size_t) k]);
                    });

            for (auto& worker: workers)
                worker.join();
        });
}

float maxDifference(const Floats& a, const Floats& b)
{
    auto largest = 0.f;

    for (auto i = 0; i < elementCount; ++i)
        largest =
            std::max(largest, std::abs(a[(std::size_t) i] - b[(std::size_t) i]));

    return largest;
}

void reportAgreement(const char* label, const Floats& a, const Floats& b)
{
    auto difference = maxDifference(a, b);
    auto agrees = difference <= tolerance;

    std::printf("  %-26s max |difference| %.3g, %s (tolerance %.0e)\n",
                label,
                (double) difference,
                agrees ? "agree" : "DISAGREE",
                (double) tolerance);

    if (!agrees)
        Apps::setReturnValue(1);
}

void runCpuCompute(bool forceCpuOnly)
{
    auto left = makeSignal(0.001f);
    auto right = makeSignal(0.0037f);

    auto kernel = CrossfadeKernel {};
    kernel.blend = 0.35f;
    kernel.gain = 0.8f;

    auto executor = CpuCompute::Executor {kernel};

    if (!executor.isValid())
    {
        std::printf("CpuCompute: the kernel cannot run on the CPU: %s\n",
                    executor.reason().c_str());
        Apps::setReturnValue(1);
        return;
    }

    const auto& plan = executor.plan();
    std::printf("CpuCompute: %d elements, %d timed runs per path\n",
                elementCount,
                repetitions);
    std::printf("  plan: %d lanes per group, %d groups per batch, "
                "footprint %zu bytes\n\n",
                plan.lanes(),
                plan.groupsPerBatch(),
                plan.footprintBytes());

    auto onCpu = Floats((std::size_t) elementCount);
    auto onThreads = Floats((std::size_t) elementCount);
    auto bindings = CpuCompute::Bindings {};
    bindings.set(kernel.left, left);
    bindings.set(kernel.right, right);
    bindings.set(kernel.output, onCpu);

    auto cpuMillis =
        millisPerRun([&] { executor.dispatch(bindings, elementCount); });
    std::printf("  cpu, one thread            %8.3f ms per dispatch\n", cpuMillis);

    auto threadCount = std::max(1, (int) std::thread::hardware_concurrency());
    auto threaded = bindings;
    threaded.set(kernel.output, onThreads);
    auto threadsMillis = runOnCpuThreads(executor, threaded, threadCount);
    std::printf("  cpu, %2d threads            %8.3f ms per dispatch "
                "(thread start included)\n",
                threadCount,
                threadsMillis);

    auto deviceValid = !forceCpuOnly && Device::shared().isValid();

    if (!deviceValid)
    {
        std::printf("  gpu: %s; the CPU path ran alone\n\n",
                    forceCpuOnly ? "skipped (--cpu-only)" : "no device came up");
        reportAgreement("cpu threads vs cpu:", onThreads, onCpu);
        return;
    }

    auto onGpu = Floats((std::size_t) elementCount);
    auto gpuMillis = runOnGpu(kernel, left, right, onGpu);
    std::printf("  gpu, commit and wait       %8.3f ms per dispatch\n\n", gpuMillis);

    reportAgreement("gpu vs cpu:", onGpu, onCpu);
    reportAgreement("cpu threads vs cpu:", onThreads, onCpu);
}
} // namespace

int main(int argc, char* argv[])
{
    auto forceCpuOnly = argc > 1 && std::strcmp(argv[1], "--cpu-only") == 0;
    return Apps::run([forceCpuOnly] { runCpuCompute(forceCpuOnly); });
}
