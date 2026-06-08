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
