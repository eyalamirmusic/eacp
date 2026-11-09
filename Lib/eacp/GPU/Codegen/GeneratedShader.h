#pragma once

/** @file GeneratedShader, the native source and vertex layout the EDSL emits for one
    graph.
*/

#include "../Pipeline/VertexLayout.h"
#include "../Shader/ShaderSource.h"
#include "ShaderGraph.h"
#include "ShaderReflection.h"

namespace eacp::GPU
{
// The output of the EDSL: the native source for the current backend (ready for
// Device::makeShaderLibrary), the vertex layout derived from the very same
// input declarations, and the reflection of the graph both were emitted from.
// This is the "factory that returns a ShaderSource" the GPU layer was designed
// around, so call sites consume it with no downstream changes.
struct GeneratedShader
{
    // The grid shape a kernel's body asked for, carried so the dispatch cannot
    // disagree with the entry point that was emitted for it. Meaningless for a
    // render shader.
    DispatchRank dispatchRank() const;

    // Which render stage reads the uniform block, from the emitter's own
    // predicates (ShaderEmitter.h), so a bind cannot disagree with the
    // signature that was emitted for it. RenderPass::draw(program) binds the
    // block to the stage that reads it and skips the one that does not - an
    // unused bind is what the validation layer reports for every such pass.
    // Both false for a kernel, which takes its uniforms as a ComputePass bind.
    bool vertexReadsUniforms() const;
    bool fragmentReadsUniforms() const;

    // Whether the vertex stage reads texture or sampler `slot`: the slots
    // ShaderProgram::bindTextures binds to the vertex stage as well as to the
    // fragment one. False for a kernel.
    bool vertexReadsTexture(int slot) const;

    ShaderSource source;
    VertexLayout vertexLayout;
    ShaderReflection reflection;
};
} // namespace eacp::GPU
