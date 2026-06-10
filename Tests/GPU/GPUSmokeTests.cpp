#include <eacp/GPU/GPU.h>

#include <eacp/Core/Platform/Platform.h>

#include <NanoTest/NanoTest.h>

#include <cstdint>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// Minimal shader whose vertex input matches the single Float4 attribute below,
// so pipeline creation exercises the full vertex-descriptor path. Provided in
// the backend the platform compiles (MSL on Metal, HLSL on D3D11).
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
    descriptor.filter = TextureFilter::Nearest;

    auto texture = device.makeTexture(descriptor, pixels);
    check(texture.isValid());
    check(texture.width() == 2);
    check(texture.height() == 2);

    auto empty = device.makeTexture(descriptor);
    check(empty.isValid());

    auto invalid = device.makeTexture(TextureDescriptor {});
    check(!invalid.isValid());
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
