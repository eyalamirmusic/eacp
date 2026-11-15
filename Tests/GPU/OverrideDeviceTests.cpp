#include "CpuCrossCheck.h"
#include "PrecompiledTestBlobs.h"

#include <eacp/GPU/Shader/PrecompiledShaders.h>

#include <cstdint>
#include <functional>
#include <string>

// Pipeline-overridable constants on a device: one kernel and one render
// program, each compiled once and prepared with two value sets, giving the two
// results the CPU executor gives for the same values; defaults where nothing
// is given; one pipeline per value set on one library, with a set given again
// - or a default given rather than left out - taking the pipeline already
// built; refused values building nothing until good ones are given; an f16
// override; and a precompiled library specialised at load. The spellings are
// pinned by OverrideCodegen/ and the resolution by CpuComputeTests' Override/.
//
// The CPU halves never self-skip; the GPU halves self-skip without a device,
// the f16 one without Device::supportsShaderFloat16, and the precompiled one
// where this machine compiles no binary (a Mac without the Metal Toolchain).

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace OverrideDeviceTests
{
namespace
{
constexpr auto threadCount = 75;
constexpr auto targetSize = 8;
constexpr auto pixelCount = targetSize * targetSize;
constexpr auto sentinel = -7.f;

struct OverrideKernel final : ComputeProgram
{
    OverrideKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto gain = overrideConstant<Float>("gain", 2.f);
        auto bias = overrideConstant<Int>("bias", -1);
        auto stride = overrideConstant<UInt>("stride");
        auto flip = overrideConstant<Bool>("flip", false);

        auto value = input[id] * gain + toFloat(bias);
        write(output, id, select(flip, -value, value));
        write(indices, id, id * stride);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UIntOutputBuffer> indices;

    EACP_SHADER(input, output, indices)
};

struct HalfKernel final : ComputeProgram
{
    HalfKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto step = overrideConstant<Half>("step", 0.25f);
        write(output, id, step * toHalf(toFloat(id)));
    }

    Uniform<HalfOutputBuffer> output;

    EACP_SHADER(output)
};

struct Expected
{
    float gain = 2.f;
    int bias = -1;
    unsigned stride = 1u;
    bool flip = false;
};

float inputAt(int index)
{
    return 0.25f * (float) index - 4.f;
}

Vector<float> inputs()
{
    auto values = Vector<float> {};

    for (auto index = 0; index < threadCount; ++index)
        values.add(inputAt(index));

    return values;
}

float outputAt(int index, const Expected& expected)
{
    const auto value = inputAt(index) * expected.gain + (float) expected.bias;
    return expected.flip ? -value : value;
}

void crossCheck(OverrideKernel& kernel,
                const OverrideValues& values,
                const Expected& expected,
                const std::source_location& where = std::source_location::current())
{
    auto verify = [&](const Readback& readback)
    {
        const auto& output = readback.floats(kernel.output);
        const auto& indices = readback.uints(kernel.indices);
        auto right = 0;

        for (auto index = 0; index < threadCount; ++index)
            if (output[index] == outputAt(index, expected)
                && indices[index] == (std::uint32_t) index * expected.stride)
                ++right;

        check(right == threadCount, readback.name(), where);
    };

    CrossCheck {kernel}
        .input(kernel.input, inputs())
        .output(kernel.output, threadCount, sentinel)
        .output(kernel.indices, threadCount)
        .overriding(values)
        .agreeing()
        .run(threadCount, verify, where);
}

// The GPU half by hand, for a program prepared or specialised before: runs
// whichever pipeline is current into fresh buffers and reads them back.
struct GpuRun
{
    explicit GpuRun(OverrideKernel& kernel)
        : input(Device::shared().makeBuffer(
              inputs().data(), threadCount * 4, BufferUsage::Storage))
        , output(Device::shared().makeBuffer(threadCount * 4, BufferUsage::Storage))
        , indices(Device::shared().makeBuffer(threadCount * 4, BufferUsage::Storage))
    {
        auto fill = Vector<float> {};
        fill.resize(threadCount, sentinel);
        output.update(fill.data(), threadCount * 4);

        kernel.input = input;
        kernel.output = output;
        kernel.indices = indices;

        auto commands = Device::shared().makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, threadCount);
        }

        commands.commit();

        floats.resize(threadCount);
        uints.resize(threadCount);
        output.read(floats.data(), threadCount * 4);
        indices.read(uints.data(), threadCount * 4);
    }

    bool matches(const Expected& expected) const
    {
        for (auto index = 0; index < threadCount; ++index)
            if (floats[index] != outputAt(index, expected)
                || uints[index] != (std::uint32_t) index * expected.stride)
                return false;

        return true;
    }

    bool untouched() const
    {
        for (auto value: floats)
            if (value != sentinel)
                return false;

        return true;
    }

    Buffer input;
    Buffer output;
    Buffer indices;
    Vector<float> floats;
    Vector<std::uint32_t> uints;
};

bool canTest()
{
    return Device::shared().isValid();
}

struct QuadVertex
{
    float position[2];
};

constexpr QuadVertex quad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

// The vertex stage scales the quad by one override; the fragment stage colours
// it from three more.
struct OverrideShader final : ShaderProgram
{
    OverrideShader()
    {
        compile();
        setVertices(quad);
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto scale = overrideConstant<Float>("scale", 1.f);
        auto shade = overrideConstant<Float>("shade");
        auto blue = overrideConstant<Bool>("blue", false);
        auto level = overrideConstant<UInt>("level", 2u);

        setPosition(float4(position * scale, 0.f, 1.f));
        setFragment(
            float4(shade, toFloat(level) * 0.125f, select(blue, 1.f, 0.f), 1.f));
    }

    EACP_SHADER()
};

struct FrameRunner final : GPUView
{
    FrameRunner() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        draw(frame);
        frame.beginPass({Graphics::Color::black()});
    }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

using FloatPixels = Array<float, pixelCount * 4>;

FloatPixels drawFloat(ShaderProgram& program)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::RGBA32Float;
    descriptor.renderTarget = true;

    auto target = Device::shared().makeTexture(descriptor);
    auto pixels = FloatPixels {};
    auto view = FrameRunner {};

    auto drawFrame = [&](Frame& frame)
    {
        {
            auto passDescriptor = RenderPassDescriptor {};
            passDescriptor.clearColor = Graphics::Color::black();

            auto pass = frame.beginPass(target, passDescriptor);
            pass.draw(program);
        }

        frame.flush();
        target.read(pixels.data());
    };

    view.draw = drawFrame;
    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    check(view.renderToImage(1.f).isValid());
    return pixels;
}

RenderPipelineDescriptor describeFloatPipeline()
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = 1;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA32Float);
    return descriptor;
}

bool pixelIs(const FloatPixels& pixels, int x, int y, float r, float g, float b)
{
    const auto at = (y * targetSize + x) * 4;
    return pixels[at] == r && pixels[at + 1] == g && pixels[at + 2] == b
           && pixels[at + 3] == 1.f;
}

// What a build would have embedded, for one case, under the require flag.
struct InjectedShaders
{
    explicit InjectedShaders(const PrecompiledTestBlobs::Resources& resources)
    {
        Precompiled::clearForTesting();
        Precompiled::setRequiredForTesting(true);

        for (const auto& resource: resources)
            Precompiled::setForTesting(resource.name, resource.bytes);
    }

    ~InjectedShaders() { Precompiled::clearForTesting(); }

    InjectedShaders(const InjectedShaders&) = delete;
    InjectedShaders& operator=(const InjectedShaders&) = delete;
};

auto tTwoSets = test("OverrideDevice/aKernelRunsWithTwoValueSets") = []
{
    auto kernel = OverrideKernel {};

    crossCheck(
        kernel,
        OverrideValues {}.set("stride", 3u).set("gain", 0.5f).set("flip", true),
        Expected {0.5f, -1, 3u, true});
    crossCheck(kernel,
               OverrideValues {}.set("stride", 7u).set("gain", -3.f).set("bias", 11),
               Expected {-3.f, 11, 7u, false});
};

auto tDefaults = test("OverrideDevice/defaultsApplyWhereNoValueIsGiven") = []
{
    auto kernel = OverrideKernel {};

    crossCheck(kernel, OverrideValues {}.set("stride", 2u), Expected {2.f, -1, 2u});
    crossCheck(kernel,
               OverrideValues {}.set("stride", 2u).set("gain", 2.f).set("bias", -1),
               Expected {2.f, -1, 2u});
};

auto tPerSet = test("OverrideDevice/oneLibraryKeepsAPipelinePerValueSet") = []
{
    if (!canTest())
        return;

    auto kernel = OverrideKernel {};
    const auto first = OverrideValues {}.set("stride", 1u).set("gain", 4.f);
    const auto second = OverrideValues {}.set("stride", 5u).set("flip", true);

    kernel.prepare(first);
    check(kernel.isValid());
    const auto* firstPipeline = &kernel.pipeline();
    check(GpuRun {kernel}.matches(Expected {4.f, -1, 1u, false}));

    check(kernel.specialize(second));
    const auto* secondPipeline = &kernel.pipeline();
    check(secondPipeline != firstPipeline, "another value set, another pipeline");
    check(secondPipeline->nativeState() != firstPipeline->nativeState());
    check(GpuRun {kernel}.matches(Expected {2.f, -1, 5u, true}));

    check(kernel.specialize(first));
    check(&kernel.pipeline() == firstPipeline, "a set given again is the same one");
    check(GpuRun {kernel}.matches(Expected {4.f, -1, 1u, false}));

    check(kernel.specialize(
        OverrideValues {}.set("stride", 5u).set("flip", true).set("gain", 2.f)));
    check(&kernel.pipeline() == secondPipeline,
          "a default given is the same set as one left out");

    kernel.prepare(first);
    check(kernel.isValid());
    check(GpuRun {kernel}.matches(Expected {4.f, -1, 1u, false}),
          "prepare() builds afresh");
};

auto tRefused = test("OverrideDevice/refusedValuesBuildNothingUntilGoodOnes") = []
{
    if (!canTest())
        return;

    auto kernel = OverrideKernel {};

    kernel.prepare();
    check(!kernel.isValid(), "an override with no default needs a value");
    check(GpuRun {kernel}.untouched(), "a refused pipeline dispatches nothing");

    check(!kernel.specialize(OverrideValues {}.set("stride", 1u).set("gian", 1.f)));
    check(!kernel.isValid());
    check(!kernel.specialize(OverrideValues {}.set("stride", 1.f)));
    check(!kernel.isValid());

    check(kernel.specialize(OverrideValues {}.set("stride", 4u)));
    check(kernel.isValid());
    check(GpuRun {kernel}.matches(Expected {2.f, -1, 4u, false}));

    auto unprepared = OverrideKernel {};
    check(!unprepared.specialize(OverrideValues {}.set("stride", 1u)),
          "specialize() needs a prepared library");
};

auto tRender = test("OverrideDevice/aRenderProgramTakesItsValuesInBothStages") = []
{
    if (!canTest())
        return;

    auto shader = OverrideShader {};

    shader.prepare(describeFloatPipeline(), OverrideValues {}.set("shade", 0.25f));
    check(shader.pipeline().isValid());
    const auto* whole = &shader.pipeline();
    const auto first = drawFloat(shader);

    check(pixelIs(first, 0, 0, 0.25f, 0.25f, 0.f), "defaults: the whole target");
    check(pixelIs(first, 4, 4, 0.25f, 0.25f, 0.f));

    check(shader.specialize(OverrideValues {}
                                .set("shade", 0.75f)
                                .set("scale", 0.5f)
                                .set("blue", true)
                                .set("level", 6u)));
    check(&shader.pipeline() != whole);
    const auto second = drawFloat(shader);

    const auto corner = (0 * targetSize + 0) * 4;
    check(second[corner] == 0.f && second[corner + 1] == 0.f
              && second[corner + 2] == 0.f,
          "the vertex stage's scale leaves the corner clear");
    check(pixelIs(second, 4, 4, 0.75f, 0.75f, 1.f));
    check(pixelIs(second, 3, 3, 0.75f, 0.75f, 1.f));

    check(shader.specialize(OverrideValues {}.set("shade", 0.25f)));
    check(&shader.pipeline() == whole);
    check(pixelIs(drawFloat(shader), 0, 0, 0.25f, 0.25f, 0.f));

    shader.prepare(describeFloatPipeline());
    check(!shader.pipeline().isValid(), "the fragment's override has no default");
};

auto tHalf = test("OverrideDevice/aHalfOverrideReachesTheDevice") = []
{
    auto kernel = HalfKernel {};

    for (auto step: {0.25f, 0.1f, -3.5f})
    {
        const auto rounded = halfToFloat(halfFromFloat(step));

        auto verify = [&](const Readback& readback)
        {
            const auto& halves = readback.halves(kernel.output);
            auto right = 0;

            for (auto index = 0; index < threadCount; ++index)
            {
                const auto product =
                    halfToFloat(halfFromFloat(rounded * (float) index));

                if (halfToFloat(halves[index]) == product)
                    ++right;
            }

            check(right == threadCount, readback.name());
        };

        auto values = OverrideValues {};

        if (step != 0.25f)
            values.set("step", step);

        CrossCheck {kernel}
            .output(kernel.output, filled(threadCount, std::uint16_t {0}))
            .overriding(values)
            .agreeing()
            .run(threadCount, verify);
    }
};

auto tPrecompiled = test("OverrideDevice/aPrecompiledKernelSpecialisesAtLoad") = []
{
    if (!canTest())
        return;

    auto kernel = OverrideKernel {};
    const auto resources = PrecompiledTestBlobs::compileAheadOfTime(kernel.source());

    if (resources.empty())
        return;

    const auto injected = InjectedShaders {resources};

    kernel.prepare(OverrideValues {}.set("stride", 6u).set("gain", -1.f));
    check(kernel.isValid());
    check(kernel.isPrecompiled(), "only the binary can have built it");
    check(GpuRun {kernel}.matches(Expected {-1.f, -1, 6u, false}));

    check(kernel.specialize(OverrideValues {}.set("stride", 2u).set("bias", 9)));
    check(kernel.isPrecompiled());
    check(GpuRun {kernel}.matches(Expected {2.f, 9, 2u, false}));
};
} // namespace
} // namespace OverrideDeviceTests
