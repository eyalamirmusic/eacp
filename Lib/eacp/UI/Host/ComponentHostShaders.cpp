#include "ComponentHostShaders.h"

#include "../Render/ImageBatch.h"
#include "../Render/LayerRenderer.h"
#include "../Render/MeshBatch.h"
#include "../Render/ShapeBatch.h"

#include <eacp/Text/GlyphRenderer.h>

namespace eacp::UI
{
void addComponentHostShaders(GPU::ShaderWarmUp& warmUp)
{
    warmUp.addGraphs("UI shapes", &ShapeBatch::forEachShaderGraph)
        .addGraphs("UI meshes", &MeshBatch::forEachShaderGraph)
        .addGraphs("UI images", &ImageBatch::forEachShaderGraph)
        .addGraphs("UI layers", &LayerRenderer::forEachShaderGraph)
        .addGraphs("glyphs", &Text::GlyphRenderer::forEachShaderGraph);
}
} // namespace eacp::UI
