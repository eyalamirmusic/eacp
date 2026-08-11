#pragma once

#include "../Pipeline/VertexLayout.h"
#include "../Shader/ShaderSource.h"
#include "ShaderGraph.h"

namespace eacp::GPU
{
// Native source for the current backend, ready for Device::makeShaderLibrary.
struct GeneratedShader
{
    ShaderSource source;
    VertexLayout vertexLayout;

    // Meaningless for a render shader.
    DispatchRank dispatchRank = DispatchRank::OneD;

    // RenderPass::draw binds the uniform block only to the stage that reads it,
    // as an unused bind is a validation-layer error. Both false for a kernel.
    bool vertexReadsUniforms = false;
    bool fragmentReadsUniforms = false;
};
} // namespace eacp::GPU
