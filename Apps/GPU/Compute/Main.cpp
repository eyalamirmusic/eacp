#include <eacp/GPU/GPU.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

using namespace eacp;
using namespace GPU;

namespace
{
constexpr auto twoPi = 2.0f * std::numbers::pi_v<float>;

// A small additive synth: three harmonics with 1/n amplitudes, evaluated per
// sample index. A generator kernel - no input buffer, the sample clock is
// toFloat(threadId()). The shared phase term feeds all three sin() calls, so
// the emitter hoists it into one named local.
struct ToneKernel final : ComputeProgram
{
    Uniform<OutputBuffer> output;
    Uniform<Float> frequency;
    Uniform<Float> sampleRate;
    EACP_SHADER(output, frequency, sampleRate)

    ToneKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto phase = toFloat(i) / sampleRate * frequency * twoPi;
        auto wave = sin(phase) + sin(phase * 2.0f) / 2.0f + sin(phase * 3.0f) / 3.0f;
        write(output, i, wave);
    }
};

// Crossfades two rendered tones, then soft-clips the blend with x / (1 + |x|)
// so the harmonic stacks stay inside [-1, 1] without a hard clamp.
struct CrossfadeKernel final : ComputeProgram
{
    Uniform<InputBuffer> toneA;
    Uniform<InputBuffer> toneB;
    Uniform<OutputBuffer> output;
    Uniform<Float> blend;
    Uniform<Float> gain;
    EACP_SHADER(toneA, toneB, output, blend, gain)

    CrossfadeKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto mixed = mix(toneA[i], toneB[i], blend);
        auto shaped = mixed / (abs(mixed) + 1.0f);
        write(output, i, shaped * gain);
    }
};

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
} // namespace

// Headless compute with the shader EDSL: two struct-authored kernels chained
// over storage buffers, no native shader strings anywhere. The tone kernel is
// dispatched twice with different uniforms and output buffers; the crossfade
// kernel then reads both renders. Each stage gets its own pass, because the
// encoder boundary is what orders a write before the read that consumes it on
// both backends (and lets D3D rebind the outputs as inputs).
int main()
{
    auto& device = Device::shared();

    if (!device.isValid())
    {
        std::printf("Compute: no GPU device available; skipping.\n");
        return 0;
    }

    constexpr auto count = 24;
    constexpr auto bytes = sizeof(float) * count;

    auto toneA = device.makeBuffer(bytes);
    auto toneB = device.makeBuffer(bytes);
    auto blended = device.makeBuffer(bytes);

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

    commands.commit();

    float a[count] = {};
    float b[count] = {};
    float out[count] = {};
    toneA.read(a, sizeof(a));
    toneB.read(b, sizeof(b));
    blended.read(out, sizeof(out));

    printWave("Compute: tone A (1 cycle, 3 harmonics)", a, count);
    printWave("Compute: tone B (2 cycles, 3 harmonics)", b, count);
    printWave("Compute: crossfaded and soft-clipped", out, count);

    return 0;
}
