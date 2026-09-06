#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

#include <string>

// The half of PathTests.cpp that is about a *render* pipeline, split out for the
// reason Tests/GPU/ShaderCompileTests.cpp was split out of ShaderCodegenTests:
// so the file it came from stays portable and runs everywhere, and this one is
// built only where a render pipeline can be built at all.
//
// Two of the three build one and assert it is valid, which the Linux backend
// cannot do until the render half lands (stage 3 of plan.md); the third reads
// the generated source and checks it against the MSL and HLSL spellings, which
// the GLSL dialect does not share - `struct Uniforms` is a `layout(std140)
// uniform Uniforms` block there, and the fragment stage writes an out variable
// rather than returning. A GLSL arm of that check belongs beside the emitter's
// own tests rather than here.

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

// The vertex-colour shader's generated source compiles through the platform
// shader compiler. Self-skips without a GPU device.
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

// The generated source carries the viewport + colour uniform block, and the
// colour is read directly by the fragment stage - no varying needed now that
// the uniform block binds to both stages. Pure string generation, in the MSL and
// HLSL spelling.
auto tFillCodegen = test("GPUWidgets/fillShaderCodegen") = []
{
    auto shader = PathFillShader {};
    const auto& source = shader.source().source;

    check(contains(source, "struct Uniforms"));
    check(contains(source, "float2 u0")); // viewport
    check(contains(source, "float4 u1")); // colour
    check(contains(source, "return uniforms.u1;")); // read per-fragment
    check(!contains(source, "v0")); // no varying in between

    check(shader.source().vertexEntry == "vertexMain");
    check(shader.source().fragmentEntry == "fragmentMain");
};

// The real generated source compiles through the platform shader compiler and a
// pipeline builds from its layout. Self-skips on hosts without a GPU device
// (matches the GPU module's codegenCompiles test).
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
