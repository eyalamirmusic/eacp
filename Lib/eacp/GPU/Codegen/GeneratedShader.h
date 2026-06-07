#pragma once

#include "../Pipeline/VertexLayout.h"
#include "../Shader/ShaderSource.h"

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
};
} // namespace eacp::GPU
