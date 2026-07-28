#pragma once

#include "../Pipeline/VertexLayout.h"
#include "../Shader/ShaderSource.h"
#include "ShaderGraph.h"

namespace eacp::GPU
{
// The output of the EDSL: the native source for the current backend (ready for
// Device::makeShaderLibrary) plus the vertex layout derived from the very same
// input declarations. This is the "factory that returns a ShaderSource" the GPU
// layer was designed around, so call sites consume it with no downstream changes.
struct GeneratedShader
{
    ShaderSource source;
    VertexLayout vertexLayout;

    // The grid shape a kernel's body asked for, carried so the dispatch cannot
    // disagree with the entry point that was emitted for it. Meaningless for a
    // render shader.
    DispatchRank dispatchRank = DispatchRank::OneD;

    // Which stage's expressions read a uniform, taken from the emitter's own
    // predicates so a bind cannot disagree with the signature that was emitted
    // for it. RenderPass::draw(program) binds the block to the stage that reads
    // it and skips the one that does not - an unused bind is what the
    // validation layer reports for every such pass. Both false for a kernel,
    // which takes its uniforms as a ComputePass bind instead.
    bool vertexReadsUniforms = false;
    bool fragmentReadsUniforms = false;
};
} // namespace eacp::GPU
