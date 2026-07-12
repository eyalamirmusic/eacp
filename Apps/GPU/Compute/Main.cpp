#include <eacp/GPU/GPU.h>
#include <algorithm>

#include <cstdio>
#include <numbers>

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
    ToneKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto phase = toFloat(i) / sampleRate * frequency * twoPi;
        auto wave = sin(phase) + sin(phase * 2.0f) / 2.0f + sin(phase * 3.0f) / 3.0f;
        write(output, i, wave);
    }

    Uniform<OutputBuffer> output;
    Uniform<Float> frequency;
    Uniform<Float> sampleRate;
    EACP_SHADER(output, frequency, sampleRate)
};

// Crossfades two rendered tones, then soft-clips the blend with x / (1 + |x|)
// so the harmonic stacks stay inside [-1, 1] without a hard clamp.
struct CrossfadeKernel final : ComputeProgram
{
    CrossfadeKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto mixed = mix(toneA[i], toneB[i], blend);
        auto shaped = mixed / (abs(mixed) + 1.0f);
        write(output, i, shaped * gain);
    }

    Uniform<InputBuffer> toneA;
    Uniform<InputBuffer> toneB;
    Uniform<OutputBuffer> output;
    Uniform<Float> blend;
    Uniform<Float> gain;
    EACP_SHADER(toneA, toneB, output, blend, gain)
};

// A 3-tap smoothing filter using index arithmetic: each sample averaged with
// its neighbours, found with uint +, -, % and the buffer length uniform. The
// blend holds whole cycles of both tones, so wrapping around the ends reads
// the periodic signal seamlessly - no edge clamping needed.
struct SmoothKernel final : ComputeProgram
{
    SmoothKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto previous = input[(i + length - 1u) % length];
        auto next = input[(i + 1u) % length];
        write(output, i, (previous + input[i] + next) / 3.0f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UInt> length;
    EACP_SHADER(input, output, length)
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
