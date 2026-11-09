#include "Common.h"
#include "PrecompiledTestBlobs.h"

#include <eacp/GPU/Shader/PrecompiledShaders.h>

// A ShaderLibrary loads the native binary a build compiled from its exact
// source when one is embedded, and compiles the text otherwise. Here the build
// is played in-process (PrecompiledTestBlobs.h) and its blobs injected with
// setForTesting under the names a build would embed them by. macOS self-skips
// the cases that need a binary when the Metal Toolchain is not installed.
//
// Every case self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace PrecompiledShaderTests
{
namespace
{
using PrecompiledTestBlobs::compileAheadOfTime;
using PrecompiledTestBlobs::resourceNamesFor;
using PrecompiledResources = PrecompiledTestBlobs::Resources;

// Bytes that are no binary of any backend: no DXBC header, no SPIR-V magic,
// nothing Metal reads as a library.
PrecompiledResources corruptResourcesFor(const ShaderSource& source)
{
    auto resources = PrecompiledResources {};
    auto garbage = Vector<std::uint8_t>(64);

    for (auto& byte: garbage)
        byte = 0xab;

    for (const auto& name: resourceNamesFor(source))
        resources.add({name, garbage});

    return resources;
}

// What a build would have embedded, for the length of one case, with the
// require flag set either way so the environment cannot decide the case.
struct InjectedShaders
{
    InjectedShaders(const PrecompiledResources& resources, bool required)
    {
        Precompiled::clearForTesting();
        Precompiled::setRequiredForTesting(required);

        for (const auto& resource: resources)
            Precompiled::setForTesting(resource.name, resource.bytes);
    }

    ~InjectedShaders() { Precompiled::clearForTesting(); }

    InjectedShaders(const InjectedShaders&) = delete;
    InjectedShaders& operator=(const InjectedShaders&) = delete;
};

GeneratedShader makeGreenShader()
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(builder.constant(0.0f), 1.0f, 0.0f, 1.0f));

    return builder.build();
}

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f;
}

// One full-viewport triangle over a red clear, so the frame is green exactly
// when the library's pipeline drew.
struct PrecompiledView final : GPUView
{
    explicit PrecompiledView(const GeneratedShader& shader)
        : library(Device::shared().makeShaderLibrary(shader.source))
        , vertexBuffer(Device::shared().makeBuffer(fullScreenTriangle))
        , pipeline(makePipeline(shader))
    {
    }

    RenderPipeline makePipeline(const GeneratedShader& shader)
    {
        setSampleCount(1);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout = shader.vertexLayout;

        return Device::shared().makeRenderPipeline(descriptor);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{1.f, 0.f, 0.f, 1.f}});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.draw(3);
    }

    static constexpr float fullScreenTriangle[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};

    ShaderLibrary library;
    Buffer vertexBuffer;
    RenderPipeline pipeline;
};

bool drawsGreen(PrecompiledView& view)
{
    view.setBounds({0.f, 0.f, 8.f, 8.f});

    auto image = view.renderToImage(1.f);

    return image.isValid() && isGreen(image.at(4, 4));
}

struct TripleKernel final : ComputeProgram
{
    TripleKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * 3.0f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};
} // namespace

// The binary is used, under the require flag so a silent fall back to the
// source would leave the library invalid rather than pass, and it draws.
auto tPrecompiledRenderLibraryDraws =
    test("Precompiled/aRenderLibraryLoadsItsBinaryAndDraws") = []
{
    if (!Device::shared().isValid())
        return;

    const auto shader = makeGreenShader();
    const auto resources = compileAheadOfTime(shader.source);

    if (resources.empty())
        return;

    const auto injected = InjectedShaders {resources, true};

    auto view = PrecompiledView {shader};

    check(view.library.isValid());
    check(view.library.isPrecompiled());
    check(view.pipeline.isValid());
    check(drawsGreen(view));
};

// A binary that will not load is the source compiled instead: a valid library,
// a pipeline that draws, and isPrecompiled false.
auto tPrecompiledCorruptFallsBack =
    test("Precompiled/aCorruptBinaryFallsBackToTheSource") = []
{
    if (!Device::shared().isValid())
        return;

    const auto shader = makeGreenShader();
    const auto injected =
        InjectedShaders {corruptResourcesFor(shader.source), false};

    auto view = PrecompiledView {shader};

    check(view.library.isValid());
    check(!view.library.isPrecompiled());
    check(view.pipeline.isValid());
    check(drawsGreen(view));
};

auto tPrecompiledCorruptRequiredIsInvalid =
    test("Precompiled/aCorruptBinaryUnderTheRequireFlagIsInvalid") = []
{
    if (!Device::shared().isValid())
        return;

    const auto shader = makeGreenShader();
    const auto injected = InjectedShaders {corruptResourcesFor(shader.source), true};

    auto library = Device::shared().makeShaderLibrary(shader.source);

    check(!library.isValid());
    check(!library.isPrecompiled());
};

auto tPrecompiledMissRequiredIsInvalid =
    test("Precompiled/aMissUnderTheRequireFlagIsInvalid") = []
{
    if (!Device::shared().isValid())
        return;

    const auto shader = makeGreenShader();
    const auto injected = InjectedShaders {{}, true};

    auto library = Device::shared().makeShaderLibrary(shader.source);

    check(!library.isValid());
    check(!library.isPrecompiled());
};

// Without the flag a miss is the source compile, exactly as before.
auto tPrecompiledMissCompilesSource = test("Precompiled/aMissCompilesTheSource") = []
{
    if (!Device::shared().isValid())
        return;

    const auto shader = makeGreenShader();
    const auto injected = InjectedShaders {{}, false};

    auto library = Device::shared().makeShaderLibrary(shader.source);

    check(library.isValid());
    check(!library.isPrecompiled());
};

// A kernel's binary loads through the same lookup, prepared under the require
// flag so only the binary can have built it, and computes.
auto tPrecompiledKernelRuns = test("Precompiled/aKernelLoadsItsBinaryAndRuns") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto kernel = TripleKernel {};
    const auto resources = compileAheadOfTime(kernel.source());

    if (resources.empty())
        return;

    const auto injected = InjectedShaders {resources, true};

    const auto values = Vector<float> {1.f, 2.f, 3.f, 4.f};
    const auto bytes =
        (std::int64_t) values.getSize() * (std::int64_t) sizeof(float);

    auto input = device.makeBuffer(values.data(), bytes, BufferUsage::Storage);
    auto output = device.makeBuffer(bytes, BufferUsage::Storage);

    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    check(kernel.isValid());

    if (!kernel.isValid())
        return;

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, values.size());
    }

    commands.commit();

    auto result = Vector<float>(values.size());
    output.read(result.data(), bytes);

    for (auto i = 0; i < values.size(); ++i)
        check(result[i] == values[i] * 3.f);
};

// The key is the emitted text and nothing else, so the build and the runtime
// agree on it without sharing anything but the source.
auto tPrecompiledKeyIsTheSourceHash = test("Precompiled/theKeyIsTheSourceHash") = []
{
    auto source = ShaderSource::msl("kernel void k() {}");

    check(Precompiled::keyFor(source)
          == Hashing::toHex(Hashing::fnv1a64("kernel void k() {}")));
    check(Precompiled::keyFor(source).size() == 16);

    check(Precompiled::spirvResourceName("0123456789abcdef",
                                         Spirv::Target::vulkan11Spirv14,
                                         Spirv::Stage::Fragment)
          == "0123456789abcdef.vk11spv14.frag.spv");
};
} // namespace PrecompiledShaderTests
