#include "../GPU/CodegenCommon.h"

#include <eacp/GPUWidgets/GPUWidgets.h>
#include <eacp/Sprites/Sprites.h>
#include <eacp/Text/Text.h>
#include <eacp/UI/UI.h>

#include <utility>

// GPUCodegenTests compiles only its own test shaders, so an emitter arm no test
// shader reached went unchecked - which is how `length(max(0.f, q))` got out.

using namespace nano;
using namespace eacp;

namespace
{
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

    UI::ShapeBatch::forEachShaderGraph(compileOne);
    UI::MeshBatch::forEachShaderGraph(compileOne);
    UI::ImageBatch::forEachShaderGraph(compileOne);
    UI::LayerRenderer::forEachShaderGraph(compileOne);

    // A renderer that stopped handing its graph over would pass on nothing.
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

    // The mask arm and the colour arm.
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
