#include "Common.h"

#include <eacp/GPU/Vulkan/VulkanTypes.h>

#include <cmath>
#include <iterator>

// A slot the shader declares and nothing was bound to holds a placeholder on
// Vulkan: the device may not leave a slot a pipeline uses empty, and without
// VK_EXT_descriptor_indexing's partially bound slots - which this backend does
// not ask for - an empty one is undefined behaviour. The placeholders are zero,
// so what each case reads back is that zero rather than whatever an unwritten
// descriptor happens to point at. Under EACP_VK_VALIDATION=1 a slot left empty
// is also a validation error.
//
// Vulkan only: Metal and D3D12 leave an unbound read undefined.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewSize = 4;
constexpr auto viewSizeInPoints = static_cast<float>(viewSize);

// How far a read-back channel may sit from the value the shader wrote: one
// step of an 8-bit target, with room.
constexpr auto channelTolerance = 0.05f;

bool isNear(float actual, float expected)
{
    return std::abs(actual - expected) < channelTolerance;
}

// The green every placeholder shader adds on top of what it samples.
constexpr auto constantGreen = 0.5f;

// What a kernel writes beside its unbound reads and writes, to show it ran.
constexpr auto ranMarker = 7.f;

struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

constexpr auto fullQuadVertexCount = static_cast<int>(std::size(fullQuad));

struct UnboundCopyKernel final : ComputeProgram
{
    UnboundCopyKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] + 1.f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct UnboundImageKernel final : ComputeProgram
{
    UnboundImageKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto one = constant(1.f);
        write(target, i, i * 0u, float4(one, one, one, one));
        write(output, i, one * ranMarker);
    }

    Uniform<WritableTexture2D> target;
    Uniform<OutputBuffer> output;

    EACP_SHADER(target, output)
};

// Both texture shapes, so the cube slot needs a cube placeholder rather than
// the 2D one, and a constant on top so the result is not the clear colour.
struct UnboundTexturesShader final : ShaderProgram
{
    UnboundTexturesShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = varying(position * 0.5f + 0.5f);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(sample(flat, uv) + sample(cube, float3(constant(1.f), 0.f, 0.f))
                    + float4(constant(0.f), constantGreen, 0.f, 1.f));
    }

    Uniform<Texture2D> flat;
    Uniform<TextureCube> cube;

    EACP_SHADER(flat, cube)
};

struct UnboundTexturesView final : GPUView
{
    UnboundTexturesView()
    {
        setSampleCount(1);
        shader.setVertices(fullQuad, fullQuadVertexCount);
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{1.f, 0.f, 0.f, 1.f}});
        pass.draw(shader);
    }

    UnboundTexturesShader shader;
};

// A kernel reading a uint storage texture nobody bound and writing a 2D-array
// one nobody bound: the read is a usampler2D over an R32_UINT placeholder, the
// write an image2DArray over an RGBA8 one, and the rest of the kernel runs.
struct UnboundIntegerAndArrayKernel final : ComputeProgram
{
    UnboundIntegerAndArrayKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto origin = int2(toInt(i * 0u), toInt(i * 0u));
        auto label = load(labels, origin);
        write(layers, origin, i * 0u, float4(constant(1.f), 1.f, 1.f, 1.f));
        write(output, i, toFloat(label.x()) + ranMarker);
    }

    Uniform<StorageTexture2D<TextureFormat::R32Uint, StorageAccess::Read>> labels;
    Uniform<StorageTexture2DArray<TextureFormat::RGBA8Unorm>> layers;
    Uniform<OutputBuffer> output;

    EACP_SHADER(labels, layers, output)
};

constexpr auto comparingNearest = TextureSampling {
    TextureFilter::Nearest, TextureAddressMode::Clamp, TextureCompare::LessEqual};

constexpr auto shadowReference = 0.5f;

// Every shape eacp's GLSL emitter declares beyond 2D and cube, all unbound: a
// sampler2DArray, a samplerCubeArray, a sampler3D and a sampler2DShadow, each
// over a placeholder of its own view type, the shadow one a depth image read
// through a comparing sampler.
struct UnboundShapesShader final : ShaderProgram
{
    UnboundShapesShader()
    {
        shadow.sampling = comparingNearest;
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = varying(position * 0.5f + 0.5f);
        auto direction = float3(constant(1.f), 0.f, 0.f);

        setPosition(float4(position, 0.f, 1.f));

        auto lit = sampleCompare(shadow, uv, constant(shadowReference));
        setFragment(sample(layers, uv, 0) + sample(cubes, direction, 0)
                    + sample(volume, float3(uv, 0.5f))
                    + float4(lit, constant(constantGreen), 0.f, 1.f));
    }

    Uniform<Texture2DArray> layers;
    Uniform<TextureCubeArray> cubes;
    Uniform<Texture3D> volume;
    Uniform<TextureDepth2D> shadow;

    EACP_SHADER(layers, cubes, volume, shadow)
};

struct UnboundShapesView final : GPUView
{
    UnboundShapesView()
    {
        setSampleCount(1);
        shader.setVertices(fullQuad, fullQuadVertexCount);
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{1.f, 0.f, 0.f, 1.f}});
        pass.draw(shader);
    }

    UnboundShapesShader shader;
};

Buffer filledWith(int count, float value)
{
    auto values = Vector<float> {};
    values.assign(count, value);
    const auto bytes = static_cast<int>(sizeof(float)) * count;
    return Device::shared().makeBuffer(values.data(), bytes, BufferUsage::Storage);
}

Vector<float> readFloats(const Buffer& buffer, int count)
{
    auto values = Vector<float> {};
    values.resize(count);
    const auto bytes = static_cast<int>(sizeof(float)) * count;
    buffer.read(values.data(), bytes);
    return values;
}

void dispatchBoundByHand(ComputePass& pass, ComputeProgram& program, int count)
{
    const auto* uniforms = program.packedUniforms(count);
    pass.setBytes(uniforms, program.uniformByteSize());
    pass.dispatch(count);
}

// Every unbound read was zero, so the centre pixel is the constant green alone.
void checkOnlyTheConstantGreenShows(GPUView& view)
{
    view.setBounds({0.f, 0.f, viewSizeInPoints, viewSizeInPoints});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    if (!image.isValid())
        return;

    const auto pixel = image.at(viewSize / 2, viewSize / 2);
    check(isNear(pixel.r, 0.f));
    check(isNear(pixel.g, constantGreen));
    check(isNear(pixel.b, 0.f));
}
} // namespace

auto tAnUnboundComputeInputReadsZero =
    test("Placeholder/anUnboundComputeInputReadsZero") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 8;
    auto output = filledWith(count, -1.f);

    auto kernel = UnboundCopyKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(kernel.pipeline());
        pass.setOutputBuffer(output, kernel.output.slot);
        dispatchBoundByHand(pass, kernel, count);
    }

    commands.commit();

    for (auto value: readFloats(output, count))
        check(value == 1.f);
};

// A kernel writing an image nobody bound writes the placeholder, and the rest
// of the kernel still runs.
auto tAnUnboundComputeOutputImageIsHarmless =
    test("Placeholder/anUnboundComputeOutputImageIsHarmless") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 4;
    auto output = filledWith(count, -1.f);

    auto kernel = UnboundImageKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(kernel.pipeline());
        pass.setOutputBuffer(output, kernel.output.slot);
        dispatchBoundByHand(pass, kernel, count);
    }

    commands.commit();

    for (auto value: readFloats(output, count))
        check(value == ranMarker);
};

auto tUnboundTexturesSampleTransparentBlack =
    test("Placeholder/unboundTexturesSampleTransparentBlack") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = UnboundTexturesView {};
    checkOnlyTheConstantGreenShows(view);
};

// The placeholder for an unbound uint storage read is a uint image, which
// reads zero; the unbound 2D-array write lands in its placeholder.
auto tUnboundIntegerAndArrayStorageTextures =
    test("Placeholder/unboundIntegerAndArrayStorageTexturesAreHarmless") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 4;
    auto output = filledWith(count, -1.f);

    auto kernel = UnboundIntegerAndArrayKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(kernel.pipeline());
        pass.setOutputBuffer(output, kernel.output.slot);
        dispatchBoundByHand(pass, kernel, count);
    }

    commands.commit();

    for (auto value: readFloats(output, count))
        check(value == ranMarker);
};

// A 2D array, a cube array, a volume and a shadow sampler left unbound each
// read zero - the comparison against a zero depth fails - so only the constant
// shows. On a device without imageCubeArray the cube-array slot has no
// placeholder; this case is then not run.
auto tUnboundShapesSampleZero =
    test("Placeholder/unboundArrayCubeArrayVolumeAndShadowSampleZero") = []
{
    if (!Device::shared().isValid() || !Device::shared().supportsCubeArrays())
        return;

    auto view = UnboundShapesView {};
    checkOnlyTheConstantGreenShows(view);
};

// A slot whose shape the device cannot make keeps an empty placeholder, which
// the passes take as "do not write this slot": a write naming a null view is
// invalid without nullDescriptor. Filled, the same slot has one.
auto tASlotNeverFilledHasNoPlaceholder =
    test("Placeholder/aSlotNeverFilledHasNoPlaceholder") = []
{
    auto bindings = VulkanTextureBindings {};
    bindings.add(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

    check(!bindings.hasPlaceholder(0));
    check(!bindings.hasPlaceholder(1), "a slot nothing declares");

    if (!Device::shared().isValid())
        return;

    getVulkanShared().fillPlaceholders(bindings);
    check(bindings.hasPlaceholder(0));
};
