#pragma once

/** @file
    ShaderSource, native shader text plus the entry points and bindings a pipeline
    needs.
*/

#include "../Common.h"

namespace eacp::GPU
{
enum class ShaderBackend
{
    Metal,
    DirectX,
    Vulkan
};

enum class ShaderStage
{
    Vertex,
    Fragment,
    Compute
};

// The threadgroup a kernel is dispatched in, chosen by its author. A default
// shape means the stock one for the kernel's rank - 64 threads in 1D, 8x8 in
// 2D, 4x4x4 in 3D - which is what ComputePass::threadGroupWidth and its two
// siblings spell.
struct ThreadGroupShape
{
    bool isSet() const;
    int threadCount() const;

    int x = 0;
    int y = 1;
    int z = 1;
};

// Native shader source plus the metadata a pipeline needs. The whole GPU layer
// downstream of this type consumes only this struct, so the shader EDSL is "a
// factory that returns a ShaderSource" with no call-site changes. What a
// source binds is not described here: the EDSL reflects its own shaders from
// the graph they were emitted from (GeneratedShader::reflection), and a
// hand-written source is taken as it is.
struct ShaderSource
{
    static ShaderSource msl(std::string sourceToUse);

    static ShaderSource hlsl(std::string sourceToUse);

    // Both stages share one string behind `#ifdef EACP_VERTEX`/`EACP_FRAGMENT`, so
    // the entry is always main; only computeEntry is still read, by isCompute().
    static ShaderSource glsl(std::string sourceToUse);

    ShaderSource& withVertex(std::string entry);

    // An empty entry makes a vertex-only source: the pipeline built from it has
    // no fragment stage, so it writes depth and stencil and no colour - a depth
    // prepass, a shadow map, an occluder pass.
    ShaderSource& withFragment(std::string entry);

    // Names the kernel entry point and marks this as a compute source: a library
    // built from it compiles only the compute stage, and ComputePipeline pulls
    // this function. Leave unset for a vertex/fragment source.
    ShaderSource& withCompute(std::string entry);

    bool isCompute() const;

    bool hasFragment() const;

    // The group the kernel's entry point was emitted for, which is the group
    // the pass has to dispatch it in. Left unset for a hand-written source, and
    // the stock shape for the dispatch's rank is used.
    ShaderSource& withThreadGroup(ThreadGroupShape shape);

    // Marks a source that declares pipeline-overridable constants
    // (PipelineConstants.h): Metal function constants, Vulkan specialization
    // constants or, on D3D, EACP_OVERRIDE_<id> macros. A pipeline built from it
    // specialises it, with the values it is given or the shader's defaults.
    // The EDSL sets it for a shader with an override.
    ShaderSource& withPipelineConstants();

    ShaderBackend backend = ShaderBackend::Metal;
    std::string source;
    std::string vertexEntry = "vertexMain";
    std::string fragmentEntry = "fragmentMain";
    std::string computeEntry; // empty unless this is a compute source
    ThreadGroupShape threadGroup;
    bool hasPipelineConstants = false;
};
} // namespace eacp::GPU
