#include "Common.h"

#include <cmath>

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
        write(output, i, one * 7.f);
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
                    + float4(constant(0.f), 0.5f, 0.f, 1.f));
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
        shader.setVertices(fullQuad, 6);
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{1.f, 0.f, 0.f, 1.f}});
        pass.draw(shader);
    }

    UnboundTexturesShader shader;
};

Buffer filledWith(int count, float value)
{
    auto values = Vector<float> {};
    values.assign(count, value);
    return Device::shared().makeBuffer(
        values.data(), (int) sizeof(float) * count, BufferUsage::Storage);
}

Vector<float> readFloats(const Buffer& buffer, int count)
{
    auto values = Vector<float> {};
    values.resize(count);
    buffer.read(values.data(), (int) sizeof(float) * count);
    return values;
}

void dispatchBoundByHand(ComputePass& pass, ComputeProgram& program, int count)
{
    pass.setBytes(program.packedUniforms(count), program.uniformByteSize());
    pass.dispatch(count);
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
        check(value == 7.f);
};

auto tUnboundTexturesSampleTransparentBlack =
    test("Placeholder/unboundTexturesSampleTransparentBlack") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = UnboundTexturesView {};
    view.setBounds({0.f, 0.f, (float) viewSize, (float) viewSize});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    if (!image.isValid())
        return;

    const auto pixel = image.at(viewSize / 2, viewSize / 2);
    const auto near = [](float a, float b) { return std::abs(a - b) < 0.05f; };

    check(near(pixel.r, 0.f));
    check(near(pixel.g, 0.5f));
    check(near(pixel.b, 0.f));
};
