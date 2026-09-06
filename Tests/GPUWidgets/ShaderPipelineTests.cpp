#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

#include <string>

// The half of PathTests.cpp that is about a *render* pipeline, split out for the
// reason Tests/GPU/ShaderCompileTests.cpp was split out of ShaderCodegenTests:
// so the file it came from stays portable and runs everywhere.
//
// Two of the three build a pipeline and assert it is valid, and self-skip
// without a device. The third reads the generated source, which is the one
// place in this directory where the dialect shows through: MSL and HLSL declare
// a `struct Uniforms` and the fragment stage returns its colour, while GLSL
// declares an interface block and writes an out variable. It asks each dialect
// for its own spelling of the same two facts - the block holds the viewport and
// the colour, and the colour is read in the fragment stage with no varying in
// between.

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
// the uniform block binds to both stages. Pure string generation, asked of
// whichever dialect this platform emits.
auto tFillCodegen = test("GPUWidgets/fillShaderCodegen") = []
{
    auto shader = PathFillShader {};
    const auto& source = shader.source().source;

    if (shader.source().backend == GPU::ShaderBackend::Vulkan)
    {
        // The GLSL spelling of the same three facts: the block is an interface
        // block at the render uniform binding rather than a struct, its members
        // are vec2/vec4, and the fragment stage writes the declared output
        // instead of returning. The varying it does not have is `vary0` here.
        check(contains(source, "uniform Uniforms"));
        check(contains(source, "vec2 u0")); // viewport
        check(contains(source, "vec4 u1")); // colour
        check(contains(source, "fragColor = uniforms.u1;")); // read per-fragment
        check(!contains(source, "vary0")); // no varying in between
    }
    else
    {
        check(contains(source, "struct Uniforms"));
        check(contains(source, "float2 u0")); // viewport
        check(contains(source, "float4 u1")); // colour
        check(contains(source, "return uniforms.u1;")); // read per-fragment
        check(!contains(source, "v0")); // no varying in between
    }

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
