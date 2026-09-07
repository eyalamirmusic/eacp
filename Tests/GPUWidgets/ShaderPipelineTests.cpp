#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}
} // namespace

auto tVertexColorCompiles = test("GPUWidgets/vertexColorShaderCompiles") = []
{
    auto& device = GPU::Device::shared();

    if (!device.isValid())
        return;

    auto shader = VertexColorShader {};

    auto library = device.makeShaderLibrary(shader.source());
    check(library.isValid());

    auto descriptor = GPU::RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout();

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

// The uniform block binds to both stages, so the colour needs no varying.
auto tFillCodegen = test("GPUWidgets/fillShaderCodegen") = []
{
    auto shader = PathFillShader {};
    const auto& source = shader.source().source;

    if (shader.source().backend == GPU::ShaderBackend::Vulkan)
    {
        check(contains(source, "uniform Uniforms"));
        check(contains(source, "vec2 u0")); // viewport
        check(contains(source, "vec4 u1")); // colour
        check(contains(source, "fragColor = uniforms.u1;"));
        check(!contains(source, "vary0"));
    }
    else
    {
        check(contains(source, "struct Uniforms"));
        check(contains(source, "float2 u0")); // viewport
        check(contains(source, "float4 u1")); // colour
        check(contains(source, "return uniforms.u1;"));
        check(!contains(source, "v0"));
    }

    check(shader.source().vertexEntry == "vertexMain");
    check(shader.source().fragmentEntry == "fragmentMain");
};

auto tFillCompiles = test("GPUWidgets/fillShaderCompiles") = []
{
    auto& device = GPU::Device::shared();

    if (!device.isValid())
        return;

    auto shader = PathFillShader {};

    auto library = device.makeShaderLibrary(shader.source());
    check(library.isValid());

    auto descriptor = GPU::RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout();

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};
