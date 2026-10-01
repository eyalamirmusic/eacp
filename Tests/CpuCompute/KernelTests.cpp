#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <cmath>
#include <cstdint>
#include <numbers>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

// The example apps' kernels, re-declared as ComputeKernels with the apps'
// bodies unchanged, run over a few thousand elements with a partial last group
// and compared against C++ written in the same operation order. The executor
// and this file are both built without contraction and call the same std::
// transcendentals, so every comparison is exact.

namespace
{
constexpr auto twoPi = 2.0f * std::numbers::pi_v<float>;

// 64 * 62 + 29: the last group runs 29 of its 64 lanes.
constexpr auto elementCount = 3997;
constexpr auto sentinel = -7.f;

Vector<float> makeFloats(int count, float value)
{
    auto values = Vector<float> {};
    values.resize(count, value);
    return values;
}

// The mix the executor computes, a + (b - a) * t, in that order.
float mixOf(float from, float to, float amount)
{
    return from + (to - from) * amount;
}

// fract as Metal defines it: never 1, however close below 1 the rounding lands.
float fractOf(float value)
{
    auto result = value - std::floor(value);
    return result >= 1.f ? 0x1.fffffep-1f : result;
}

bool tailUntouched(const Vector<float>& values, int from)
{
    for (auto i = from; i < values.size(); ++i)
        if (values[i] != sentinel)
            return false;

    return true;
}

struct ToneKernel final : ComputeKernel
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

struct CrossfadeKernel final : ComputeKernel
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

struct SmoothKernel final : ComputeKernel
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

struct MixKernel final : ComputeKernel
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

struct IntegrateParticles final : ComputeKernel
{
    IntegrateParticles() { compile(); }

    void define() override
    {
        auto index = threadId();
        auto particle = state.read4(index);
        auto px = particle.x();
        auto py = particle.y();
        auto vx = particle.z();
        auto vy = particle.w();
        auto stiffness =
            pull * (0.25f + fract(toFloat(index) * 0.6180339887f) * 2.f);
        auto nvx = (vx + (attractorX - px) * stiffness * timeStep) * damping;
        auto nvy = (vy + (attractorY - py) * stiffness * timeStep) * damping;
        write(
            next, index, float4(px + nvx * timeStep, py + nvy * timeStep, nvx, nvy));
    }

    Uniform<InputBuffer> state;
    Uniform<OutputBuffer> next;
    Uniform<Float> attractorX;
    Uniform<Float> attractorY;
    Uniform<Float> pull;
    Uniform<Float> damping;
    Uniform<Float> timeStep;
    EACP_SHADER(state, next, attractorX, attractorY, pull, damping, timeStep)
};

float toneAt(int index, float frequency, float sampleRate)
{
    auto phase = (float) (std::uint32_t) index / sampleRate * frequency * twoPi;
    return std::sin(phase) + std::sin(phase * 2.0f) / 2.0f
           + std::sin(phase * 3.0f) / 3.0f;
}

Vector<float> toneOf(float frequency, float sampleRate)
{
    auto values = Vector<float> {};

    for (auto i = 0; i < elementCount; ++i)
        values.add(toneAt(i, frequency, sampleRate));

    return values;
}

float mixAt(int index, float scale, float gain)
{
    auto x = (float) (std::uint32_t) index * scale;
    auto wave = std::sin(x) * std::cos(x * 1.7f) + std::sin(x * 0.3f) * 0.5f;
    auto shaped = wave / (std::abs(wave) + 1.0f);
    return shaped * gain;
}
} // namespace

auto tTone = test("Kernel/toneMatchesItsReference") = []
{
    auto kernel = ToneKernel {};
    kernel.frequency = 440.f;
    kernel.sampleRate = 48000.f;

    auto executor = Executor {kernel};
    check(executor.isValid());

    auto output = makeFloats(elementCount + 35, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, elementCount));

    for (auto i = 0; i < elementCount; ++i)
        check(output[i] == toneAt(i, 440.f, 48000.f));

    check(tailUntouched(output, elementCount));
};

auto tCrossfade = test("Kernel/crossfadeMatchesItsReference") = []
{
    auto toneA = toneOf(440.f, 48000.f);
    auto toneB = toneOf(660.f, 48000.f);

    auto kernel = CrossfadeKernel {};
    kernel.blend = 0.3f;
    kernel.gain = 0.8f;

    auto executor = Executor {kernel};
    check(executor.isValid());

    auto output = makeFloats(elementCount + 35, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.toneA, toneA);
    bindings.set(kernel.toneB, toneB);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, elementCount));

    for (auto i = 0; i < elementCount; ++i)
    {
        auto mixed = mixOf(toneA[i], toneB[i], 0.3f);
        auto shaped = mixed / (std::abs(mixed) + 1.0f);
        check(output[i] == shaped * 0.8f);
    }

    check(tailUntouched(output, elementCount));
};

auto tSmooth = test("Kernel/smoothWrapsAtBothEnds") = []
{
    auto input = Vector<float> {};

    for (auto i = 0; i < elementCount; ++i)
        input.add(std::sin((float) i * 0.37f) * 3.f + (float) (i % 7));

    auto kernel = SmoothKernel {};
    kernel.length = (std::uint32_t) elementCount;

    auto executor = Executor {kernel};
    check(executor.isValid());

    auto output = makeFloats(elementCount + 35, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, elementCount));

    auto length = (std::uint32_t) elementCount;

    for (auto i = 0u; i < length; ++i)
    {
        auto previous = input[(int) ((i + length - 1u) % length)];
        auto next = input[(int) ((i + 1u) % length)];
        check(output[(int) i] == (previous + input[(int) i] + next) / 3.0f);
    }

    check(tailUntouched(output, elementCount));
};

auto tMix = test("Kernel/asyncComputeMixMatchesItsReference") = []
{
    auto kernel = MixKernel {};
    kernel.scale = 0.001f;
    kernel.gain = 0.9f;

    auto executor = Executor {kernel};
    check(executor.isValid());

    auto output = makeFloats(elementCount + 35, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, elementCount));

    for (auto i = 0; i < elementCount; ++i)
        check(output[i] == mixAt(i, 0.001f, 0.9f));

    check(tailUntouched(output, elementCount));

    kernel.scale = 0.05f;
    kernel.gain = -0.5f;
    check(executor.dispatch(bindings, elementCount));

    for (auto i = 0; i < elementCount; ++i)
        check(output[i] == mixAt(i, 0.05f, -0.5f));
};

auto tParticles = test("Kernel/integrateParticlesMatchesItsReference") = []
{
    constexpr auto attractorX = 0.2f;
    constexpr auto attractorY = -0.35f;
    constexpr auto pull = 3.5f;
    constexpr auto damping = 0.985f;
    constexpr auto timeStep = 1.f / 60.f;

    auto state = Vector<float> {};

    for (auto i = 0; i < elementCount; ++i)
    {
        auto angle = (float) i * 0.013f;
        state.add(std::cos(angle) * 0.8f);
        state.add(std::sin(angle) * 0.6f);
        state.add(std::sin(angle * 3.f) * 0.1f);
        state.add(std::cos(angle * 5.f) * -0.1f);
    }

    auto kernel = IntegrateParticles {};
    kernel.attractorX = attractorX;
    kernel.attractorY = attractorY;
    kernel.pull = pull;
    kernel.damping = damping;
    kernel.timeStep = timeStep;

    auto executor = Executor {kernel};
    check(executor.isValid());

    auto next = makeFloats(elementCount * 4 + 35, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.state, state);
    bindings.set(kernel.next, next);
    check(executor.dispatch(bindings, elementCount));

    for (auto i = 0; i < elementCount; ++i)
    {
        auto px = state[i * 4 + 0];
        auto py = state[i * 4 + 1];
        auto vx = state[i * 4 + 2];
        auto vy = state[i * 4 + 3];
        auto stiffness =
            pull
            * (0.25f + fractOf((float) (std::uint32_t) i * 0.6180339887f) * 2.f);
        auto nvx = (vx + (attractorX - px) * stiffness * timeStep) * damping;
        auto nvy = (vy + (attractorY - py) * stiffness * timeStep) * damping;

        check(next[i * 4 + 0] == px + nvx * timeStep);
        check(next[i * 4 + 1] == py + nvy * timeStep);
        check(next[i * 4 + 2] == nvx);
        check(next[i * 4 + 3] == nvy);
    }

    check(tailUntouched(next, elementCount * 4));
};

auto tComputeChain = test("Kernel/computeExampleChainsThreeKernels") = []
{
    auto toneKernelA = ToneKernel {};
    auto toneKernelB = ToneKernel {};
    auto crossfade = CrossfadeKernel {};
    auto smooth = SmoothKernel {};

    toneKernelA.frequency = 220.f;
    toneKernelA.sampleRate = 44100.f;
    toneKernelB.frequency = 330.f;
    toneKernelB.sampleRate = 44100.f;
    crossfade.blend = 0.5f;
    crossfade.gain = 0.7f;
    smooth.length = (std::uint32_t) elementCount;

    auto toneA = makeFloats(elementCount, 0.f);
    auto toneB = makeFloats(elementCount, 0.f);
    auto faded = makeFloats(elementCount, 0.f);
    auto smoothed = makeFloats(elementCount, 0.f);

    auto runA = Executor {toneKernelA};
    auto runB = Executor {toneKernelB};
    auto runFade = Executor {crossfade};
    auto runSmooth = Executor {smooth};

    auto bindA = Bindings {};
    bindA.set(toneKernelA.output, toneA);
    auto bindB = Bindings {};
    bindB.set(toneKernelB.output, toneB);
    auto bindFade = Bindings {};
    bindFade.set(crossfade.toneA, toneA);
    bindFade.set(crossfade.toneB, toneB);
    bindFade.set(crossfade.output, faded);
    auto bindSmooth = Bindings {};
    bindSmooth.set(smooth.input, faded);
    bindSmooth.set(smooth.output, smoothed);

    check(runA.dispatch(bindA, elementCount));
    check(runB.dispatch(bindB, elementCount));
    check(runFade.dispatch(bindFade, elementCount));
    check(runSmooth.dispatch(bindSmooth, elementCount));

    auto expected = makeFloats(elementCount, 0.f);

    for (auto i = 0; i < elementCount; ++i)
    {
        auto mixed =
            mixOf(toneAt(i, 220.f, 44100.f), toneAt(i, 330.f, 44100.f), 0.5f);
        expected[i] = mixed / (std::abs(mixed) + 1.0f) * 0.7f;
    }

    auto length = (std::uint32_t) elementCount;

    for (auto i = 0u; i < length; ++i)
    {
        auto previous = expected[(int) ((i + length - 1u) % length)];
        auto next = expected[(int) ((i + 1u) % length)];
        check(smoothed[(int) i] == (previous + expected[(int) i] + next) / 3.0f);
    }
};
