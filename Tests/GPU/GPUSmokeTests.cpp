#include <eacp/GPU/GPU.h>

#include <eacp/Core/Platform/Platform.h>

#include <NanoTest/NanoTest.h>

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
