#pragma once

#include <eacp/GPU/GPU.h>

#include <numbers>

// The example's three kernels, in a header of their own so Shaders.cpp can hand
// their graphs to the build-time compiler while Main.cpp dispatches them.

namespace ComputeExample
{
using namespace eacp;
using namespace GPU;

inline constexpr auto twoPi = 2.0f * std::numbers::pi_v<float>;

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
} // namespace ComputeExample
