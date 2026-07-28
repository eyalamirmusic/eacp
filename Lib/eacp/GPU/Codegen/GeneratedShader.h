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

    // Whether the kernel waits for its group. Carried for the same reason the
    // rank is: it constrains the dispatch, and the check belongs where the
    // dispatch is rather than in the head of whoever wrote the kernel.
    bool usesBarriers = false;
};
} // namespace eacp::GPU
