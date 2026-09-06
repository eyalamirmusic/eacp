#include "../GPU/CodegenCommon.h"

#include <eacp/GPUWidgets/GPUWidgets.h>
#include <eacp/Sprites/Sprites.h>
#include <eacp/Text/Text.h>
#include <eacp/UI/UI.h>

#include <utility>

// Every shader the modules above the GPU layer build, emitted as GLSL and
// handed to glslang -- wherever eacp-spirv is built, which is every Linux lane
// and any other platform that opts into EACP_BUILD_SPIRV, not only the one
// whose backend is Vulkan.
//
// The gap this closes: GPUCodegenTests compiles the GLSL of its *own* test
// shaders, so an emitter arm no test shader happened to reach was checked by
// nothing until a Vulkan device compiled a module's shader at runtime. That is
// how `length(max(0.f, q))` in the UI shape shader -- a scalar in front of a
// vector, which GLSL's max has no overload for -- got as far as sixteen failing
// pixel tests on Linux with a green macOS build behind it.
//
// Nothing here restates a shader. The graphs come from the modules themselves,
// either through forEachShaderGraph (where the program is a type nested in a
// .cpp) or from the program types the module already declares in a header, so
// editing one of those shaders is compiled here on the next run.

using namespace nano;
using namespace eacp;

namespace
{
// One program of a module, built and compiled as GLSL. Programs record their
// graph in the constructor and touch no Device, so this needs neither a device
// nor a display.
template <typename Program, typename... Args>
void expectProgramCompiles(Args&&... arguments)
{
    auto program = Program {std::forward<Args>(arguments)...};
    expectGlslCompiles(program.graph());
}

auto tUIShaders = test("ModuleShaders/uiCompilesAsGlsl") = []
{
    auto seen = 0;

    auto compileOne = [&seen](const GPU::ShaderGraph& graph)
    {
        ++seen;
        expectGlslCompiles(graph);
    };

    // The whole of the tier: MaskCache and CoverageAtlas build no shader of
    // their own, a mask being rasterized by GPUWidgets' path kernels below and
    // drawn by ShapeBatch here.
    UI::ShapeBatch::forEachShaderGraph(compileOne);
    UI::MeshBatch::forEachShaderGraph(compileOne);
    UI::ImageBatch::forEachShaderGraph(compileOne);
    UI::LayerRenderer::forEachShaderGraph(compileOne);

    // A renderer that stopped handing its shader over would otherwise leave
    // this passing on nothing at all.
    check(seen == 4);
};

auto tTextShaders = test("ModuleShaders/textCompilesAsGlsl") = []
{
    auto seen = 0;

    Text::GlyphRenderer::forEachShaderGraph(
        [&seen](const GPU::ShaderGraph& graph)
        {
            ++seen;
            expectGlslCompiles(graph);
        });

    // The mask arm and the colour arm, which are one body switched at build
    // time -- so a regression in either one is a regression this must see.
    check(seen == 2);
};

auto tSpriteShaders = test("ModuleShaders/spritesCompileAsGlsl") = []
{
    expectProgramCompiles<Sprites::SpriteShader>(GPU::TextureSampling {});
    expectProgramCompiles<Sprites::Nv12Shader>(GPU::TextureSampling {});
};

auto tGPUWidgetsShaders = test("ModuleShaders/gpuWidgetsCompilesAsGlsl") = []
{
    expectProgramCompiles<GPUWidgets::PathFillShader>();
    expectProgramCompiles<GPUWidgets::VertexColorShader>();
    expectProgramCompiles<GPUWidgets::CoverageShader>();
};

// The path rasterizer's kernels, which are the compute half of the sweep: a
// kernel's GLSL is one unguarded main() rather than the two stages behind
// macros, so it is the other shape the emitter prints.
auto tPathKernels = test("ModuleShaders/pathKernelsCompileAsGlsl") = []
{
    expectProgramCompiles<GPUWidgets::BinKernel>();
    expectProgramCompiles<GPUWidgets::ClearKernel>();
    expectProgramCompiles<GPUWidgets::BackdropScanKernel>();
    expectProgramCompiles<GPUWidgets::CoverageKernel>();
    expectProgramCompiles<GPUWidgets::ScanBlockKernel>();
    expectProgramCompiles<GPUWidgets::ScanAddKernel>();
};
} // namespace
