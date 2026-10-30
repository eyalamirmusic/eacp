#include "Kernels.h"

#include <eacp/GPU/Shader/PrecompiledShaders.h>

#include <algorithm>
#include <cstdio>
#include <initializer_list>

using namespace ComputeExample;

namespace
{
// Whether the build compiled the kernels ahead of time (Shaders.cpp and
// eacp_add_precompiled_shaders) or the device compiled their source just now.
void logKernelOrigin(std::initializer_list<const ComputeProgram*> kernels)
{
    auto precompiled = std::ranges::count_if(kernels,
                                             [](const ComputeProgram* kernel)
                                             { return kernel->isPrecompiled(); });
    auto total = std::ssize(kernels);

    LOG("Compute: ",
        precompiled,
        " of ",
        total,
        " kernels precompiled, ",
        total - precompiled,
        " compiled at runtime (",
        Precompiled::embeddedCount(),
        " precompiled blobs embedded)");
}

void printWave(const char* name, const float* samples, int count)
{
    std::printf("%s\n", name);

    constexpr auto width = 41;

    for (auto i = 0; i < count; ++i)
    {
        auto column =
            (int) std::lround((samples[i] + 1.0f) * 0.5f * (float) (width - 1));

        auto bar = std::string(width, ' ');
        bar[width / 2] = '|';
        bar[(std::size_t) std::clamp(column, 0, width - 1)] = '*';

        std::printf("  %+.3f  %s\n", (double) samples[i], bar.c_str());
    }

    std::printf("\n");
}

// Compute with the shader EDSL: two struct-authored kernels chained over
// storage buffers, no native shader strings anywhere. The tone kernel is
// dispatched twice with different uniforms and output buffers; the crossfade
// kernel then reads both renders. Each stage gets its own pass, because the
// encoder boundary is what orders a write before the read that consumes it on
// both backends (and lets D3D rebind the outputs as inputs).
void runCompute()
{
    auto& device = Device::shared();

    if (!device.isValid())
    {
        std::printf("Compute: no GPU device available; skipping.\n");
        return;
    }

    constexpr auto count = 24;
    constexpr auto bytes = sizeof(float) * count;

    auto toneA = device.makeBuffer(bytes);
    auto toneB = device.makeBuffer(bytes);
    auto blended = device.makeBuffer(bytes);
    auto smoothed = device.makeBuffer(bytes);

    auto tone = ToneKernel {};
    tone.sampleRate = (float) count;
    tone.prepare();

    auto crossfade = CrossfadeKernel {};
    crossfade.toneA = toneA;
    crossfade.toneB = toneB;
    crossfade.output = blended;
    crossfade.blend = 0.5f;
    crossfade.gain = 0.9f;
    crossfade.prepare();

    auto smooth = SmoothKernel {};
    smooth.input = blended;
    smooth.output = smoothed;
    smooth.length = count;
    smooth.prepare();

    logKernelOrigin({&tone, &crossfade, &smooth});

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();

        tone.output = toneA;
        tone.frequency = 1.0f;
        pass.dispatch(tone, count);

        tone.output = toneB;
        tone.frequency = 2.0f;
        pass.dispatch(tone, count);
    }

    {
        auto pass = commands.beginCompute();
        pass.dispatch(crossfade, count);
    }

    {
        auto pass = commands.beginCompute();
        pass.dispatch(smooth, count);
    }

    commands.commit();

    float a[count] = {};
    float b[count] = {};
    float out[count] = {};
    float soft[count] = {};
    toneA.read(a, sizeof(a));
    toneB.read(b, sizeof(b));
    blended.read(out, sizeof(out));
    smoothed.read(soft, sizeof(soft));

    printWave("Compute: tone A (1 cycle, 3 harmonics)", a, count);
    printWave("Compute: tone B (2 cycles, 3 harmonics)", b, count);
    printWave("Compute: crossfaded and soft-clipped", out, count);
    printWave("Compute: smoothed (3-tap wrap-around average)", soft, count);
}
} // namespace

int main()
{
    return Apps::run(runCompute);
}
