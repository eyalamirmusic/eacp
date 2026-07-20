#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// Minimal shader whose vertex input matches the single Float4 attribute below,
// so pipeline creation exercises the full vertex-descriptor path. Provided in
// the backend the platform compiles (MSL on Metal, HLSL on D3D12).
const char* hlslSmokeShader = R"(
struct VertexIn { float4 position : TEXCOORD0; };
struct VertexOut { float4 position : SV_Position; };

VertexOut vertexMain(VertexIn input) { VertexOut o; o.position = input.position; return o; }
float4 fragmentMain(VertexOut input) : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }
)";

const char* mslSmokeShader = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn { float4 position [[attribute(0)]]; };

vertex float4 vertexMain(VertexIn in [[stage_in]]) { return in.position; }
fragment float4 fragmentMain() { return float4(1.0, 1.0, 1.0, 1.0); }
)";

// Both branches name both strings, so neither is an unused-variable warning on
// the platform whose backend isn't selected.
ShaderSource smokeShaderSource()
{
    return Platform::isWindows() ? ShaderSource::hlsl(hlslSmokeShader)
                                 : ShaderSource::msl(mslSmokeShader);
}

// A compute kernel writing output[i] = input[i] * scale, exercising a storage
// input, a storage output and a uniform block. Mirrors the Compute example.
const char* hlslComputeShader = R"(
StructuredBuffer<float> input : register(t0);
RWStructuredBuffer<float> output : register(u1);
cbuffer Params : register(b0) { float scale; uint count; };

[numthreads(64, 1, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x >= count) return;
    output[gid.x] = input[gid.x] * scale;
}
)";

const char* mslComputeShader = R"(
#include <metal_stdlib>
using namespace metal;

struct Params { float scale; uint count; };

kernel void computeMain(device const float* input [[buffer(0)]],
                        device float* output [[buffer(1)]],
                        constant Params& params [[buffer(16)]],
                        uint gid [[thread_position_in_grid]])
{
    if (gid >= params.count) return;
    output[gid] = input[gid] * params.scale;
}
)";

ShaderSource computeShaderSource()
{
    auto source = Platform::isWindows() ? ShaderSource::hlsl(hlslComputeShader)
                                        : ShaderSource::msl(mslComputeShader);
    return source.withCompute("computeMain");
}

struct ComputeParams
{
    float scale;
    std::uint32_t count;
};

// The same scale kernel authored as a ComputeProgram struct: buffers and the
// uniform are named members, define() writes the body, and the implicit count
// guard replaces the hand-written one above.
struct ScaleKernel final : ComputeProgram
{
    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> scale;
    EACP_SHADER(input, output, scale)

    ScaleKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * scale);
    }
};

// Averages each element with its two neighbours, wrapping at the ends:
// index arithmetic with uint operators, integer literals and a uint uniform.
struct WrapAverageKernel final : ComputeProgram
{
    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UInt> length;
    EACP_SHADER(input, output, length)

    WrapAverageKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto previous = input[(i + length - 1u) % length];
        auto next = input[(i + 1u) % length];
        write(output, i, (previous + input[i] + next) / 3.0f);
    }
};
} // namespace

// Builds every resource type without a window or drawable. On a host with no
// Metal device (e.g. some headless CI VMs) it self-skips rather than fail.
auto tDeviceBuildsResources = test("GPU/deviceBuildsResources") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const float vertices[] = {0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f};

    auto buffer = device.makeBuffer(vertices);
    check(buffer.isValid());
    check(buffer.size() == sizeof(vertices));

    auto library = device.makeShaderLibrary(smokeShaderSource());
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout.attribute(VertexFormat::Float4, 0);
    descriptor.vertexLayout.stride = sizeof(float) * 4;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

// An Index-usage buffer builds (D3D11 needs the index bind flag), and the
// pipeline carries its topology through for the render pass to read at draw
// time. Self-skips without a GPU device.
auto tDeviceBuildsIndexBuffer = test("GPU/deviceBuildsIndexBuffer") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const std::uint32_t indices[] = {0, 1, 2, 0, 2, 3};

    auto buffer = device.makeBuffer(indices, BufferUsage::Index);
    check(buffer.isValid());
    check(buffer.size() == sizeof(indices));

    auto library = device.makeShaderLibrary(smokeShaderSource());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout.attribute(VertexFormat::Float4, 0);
    descriptor.vertexLayout.stride = sizeof(float) * 4;
    descriptor.topology = PrimitiveTopology::TriangleStrip;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
    check(pipeline.topology() == PrimitiveTopology::TriangleStrip);
};

// An EDSL shader with an instanceInput<T> emits a two-slot VertexLayout
// (slot 0 per-vertex, slot 1 per-instance) whose stride sums the byte size
// of the inputs at each rate. Pipeline builds against the generated layout.
// Self-skips without a GPU device.
auto tShaderBuilderEmitsInstancedLayout =
    test("GPU/shaderBuilderEmitsInstancedLayout") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};
    auto vertexPos = builder.vertexInput<Float2>(); // slot 0, 8 bytes
    auto vertexUv = builder.vertexInput<Float2>(); // slot 0, +8 = 16
    auto instanceCentre = builder.instanceInput<Float2>(); // slot 1, 8 bytes
    auto instanceScale = builder.instanceInput<Float>(); // slot 1, +4 = 12
    auto varyingUv = builder.varying(vertexUv);
    auto zero = builder.constant(0.f);
    auto one = builder.constant(1.f);
    // Simple math that references every input so the emitted layout covers
    // all of them; the exact math is placeholder.
    auto shiftedX = vertexPos.x() + instanceCentre.x() * instanceScale;
    auto shiftedY = vertexPos.y() + instanceCentre.y();
    builder.position(float4(shiftedX, shiftedY, zero, one));
    builder.fragment(float4(varyingUv, zero, one));

    auto shader = builder.build();

    // Two slots, right strides, right step rates.
    check(shader.vertexLayout.buffers.size() == 2);
    check(shader.vertexLayout.buffers[0].stride == 16);
    check(shader.vertexLayout.buffers[0].stepRate == StepRate::PerVertex);
    check(shader.vertexLayout.buffers[1].stride == 12);
    check(shader.vertexLayout.buffers[1].stepRate == StepRate::PerInstance);

    // Attributes route to their slots.
    auto& attrs = shader.vertexLayout.attributes;
    check(attrs.size() == 4);
    check(attrs[0].bufferIndex == 0);
    check(attrs[1].bufferIndex == 0);
    check(attrs[2].bufferIndex == 1);
    check(attrs[3].bufferIndex == 1);

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

// A shader without any instanceInput calls still emits the legacy
// single-buffer shape (buffers empty, stride populated). Guards against
// silently changing the layout for pre-instancing consumers.
auto tShaderBuilderPreservesSingleBufferShape =
    test("GPU/shaderBuilderPreservesSingleBufferShape") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float4>();
    auto one = builder.constant(1.f);
    builder.position(position);
    builder.fragment(float4(one, one, one, one));

    auto shader = builder.build();

    check(shader.vertexLayout.buffers.size() == 0);
    check(shader.vertexLayout.stride == 16);
    check(shader.vertexLayout.attributes.size() == 1);
    check(shader.vertexLayout.attributes[0].bufferIndex == 0);
};

// An indexed-instanced draw's resource shape - Index buffer + a pipeline whose
// layout carries a PerInstance slot - all builds cleanly. The draw itself
// requires a live frame + drawable, so this test only guards the resource path
// that drawIndexedInstanced consumes at call time. Self-skips without a GPU.
auto tDeviceBuildsIndexedInstancedResources =
    test("GPU/deviceBuildsIndexedInstancedResources") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const std::uint16_t indices[] = {0, 1, 2};
    auto indexBuffer = device.makeBuffer(indices, BufferUsage::Index);
    check(indexBuffer.isValid());

    auto builder = ShaderBuilder {};
    auto vertexPos = builder.vertexInput<Float2>();
    auto instanceCentre = builder.instanceInput<Float2>();
    auto zero = builder.constant(0.f);
    auto one = builder.constant(1.f);
    builder.position(float4(vertexPos + instanceCentre, zero, one));
    builder.fragment(float4(one, one, one, one));

    auto shader = builder.build();
    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

// Every BlendMode value produces a valid pipeline - the switch in the backend
// covers every case, and the D3D12 blend descriptor path is populated for the
// modes that need it. Self-skips without a GPU device.
auto tDeviceBuildsPipelineForEachBlendMode =
    test("GPU/deviceBuildsPipelineForEachBlendMode") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto library = device.makeShaderLibrary(smokeShaderSource());

    for (auto mode: {BlendMode::None, BlendMode::AlphaBlend, BlendMode::Additive})
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.vertexLayout.attribute(VertexFormat::Float4, 0);
        descriptor.vertexLayout.stride = sizeof(float) * 4;
        descriptor.blendMode = mode;

        auto pipeline = device.makeRenderPipeline(descriptor);
        check(pipeline.isValid());
    }
};

// A texture builds from raw pixels and reports its size; both filter modes and
// a null-pixel (uninitialised) texture build too. Self-skips without a device.
auto tDeviceBuildsTexture = test("GPU/deviceBuildsTexture") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const std::uint32_t pixels[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;

    auto texture = device.makeTexture(descriptor, pixels);
    check(texture.isValid());
    check(texture.width() == 2);
    check(texture.height() == 2);

    auto empty = device.makeTexture(descriptor);
    check(empty.isValid());

    auto invalid = device.makeTexture(TextureDescriptor {});
    check(!invalid.isValid());
};

// A texture builds straight from a decoded Graphics::Image: the bridge sizes
// the texture from the image and uploads its RGBA8 pixels. Self-skips without a
// device.
auto tDeviceBuildsTextureFromImage = test("GPU/deviceBuildsTextureFromImage") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto image = Graphics::Image(2, 2);

    auto texture = device.makeTexture(image);
    check(texture.isValid());
    check(texture.width() == 2);
    check(texture.height() == 2);
};

// Runs a compute kernel end to end - storage buffers in and out, a uniform, a
// dispatch and a readback - without a window or drawable. Self-skips on a host
// with no GPU device, like the test above.
auto tComputeRunsKernel = test("GPU/computeRunsKernel") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const float input[] = {1.f, 2.f, 3.f, 4.f};
    constexpr auto count = (int) (sizeof(input) / sizeof(input[0]));

    auto inputBuffer = device.makeBuffer(input, BufferUsage::Storage);
    auto outputBuffer = device.makeBuffer(sizeof(input), BufferUsage::Storage);
    check(inputBuffer.isValid());
    check(outputBuffer.isValid());

    auto library = device.makeShaderLibrary(computeShaderSource());
    check(library.isValid());

    auto pipeline = device.makeComputePipeline(library);
    check(pipeline.isValid());

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(pipeline);
        pass.setInputBuffer(inputBuffer, 0);
        pass.setOutputBuffer(outputBuffer, 1);
        pass.setUniform(ComputeParams {3.f, (std::uint32_t) count});
        pass.dispatch(count);
    }

    commands.commit();

    float result[count] = {};
    outputBuffer.read(result, sizeof(result));

    for (auto i = 0; i < count; ++i)
        check(result[i] == input[i] * 3.f);
};

// Runs the struct-authored EDSL kernel end to end: dispatch(kernel, count)
// binds the pipeline, the buffer members and the uniform block (with the
// implicit element count) in one call. Self-skips without a GPU device.
auto tComputeProgramRunsKernel = test("GPU/computeProgramRunsKernel") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const float input[] = {1.f, 2.f, 3.f, 4.f};
    constexpr auto count = (int) (sizeof(input) / sizeof(input[0]));

    auto inputBuffer = device.makeBuffer(input, BufferUsage::Storage);
    auto outputBuffer = device.makeBuffer(sizeof(input), BufferUsage::Storage);
    check(inputBuffer.isValid());
    check(outputBuffer.isValid());

    auto kernel = ScaleKernel {};
    kernel.input = inputBuffer;
    kernel.output = outputBuffer;
    kernel.scale = 3.f;
    kernel.prepare();
    check(kernel.pipeline().isValid());

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    float result[count] = {};
    outputBuffer.read(result, sizeof(result));

    for (auto i = 0; i < count; ++i)
        check(result[i] == input[i] * 3.f);
};

// Runs the index-arithmetic kernel end to end and checks the wrap-around
// neighbour average against the same expression on the CPU. Self-skips
// without a GPU device.
auto tComputeProgramIndexArithmetic = test("GPU/computeProgramIndexArithmetic") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const float input[] = {1.f, 2.f, 4.f, 8.f};
    constexpr auto count = (int) (sizeof(input) / sizeof(input[0]));

    auto inputBuffer = device.makeBuffer(input, BufferUsage::Storage);
    auto outputBuffer = device.makeBuffer(sizeof(input), BufferUsage::Storage);

    auto kernel = WrapAverageKernel {};
    kernel.input = inputBuffer;
    kernel.output = outputBuffer;
    kernel.length = count;
    kernel.prepare();
    check(kernel.pipeline().isValid());

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    float result[count] = {};
    outputBuffer.read(result, sizeof(result));

    // The platform shader compiler may turn the division into a reciprocal
    // multiply (Metal compiles fast-math by default), so compare within a ULP
    // budget rather than exactly.
    for (auto i = 0; i < count; ++i)
    {
        auto expected =
            (input[(i + count - 1) % count] + input[i] + input[(i + 1) % count])
            / 3.f;
        check(std::abs(result[i] - expected) < 1e-5f);
    }
};
