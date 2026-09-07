#pragma once

// Device-free, so GPUCodegenTests links eacp-gpu-codegen alone.
#include <eacp/Core/Platform/Platform.h>
#include <eacp/GPU/Codegen/ShaderBindings.h>
#include <eacp/GPU/Codegen/ShaderBuilder.h>
#include <eacp/GPU/Codegen/ShaderEmitter.h>
#include <eacp/GPU/Codegen/UniformLayout.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/GPU/Frame/RenderPass.h>

#include <NanoTest/NanoTest.h>

#ifdef EACP_HAS_SPIRV
#include <eacp/GPU/Spirv/SpirvCompiler.h>
#endif

#include <source_location>
#include <string>

// Where eacp-spirv is built (Linux by default) the emitted GLSL is compiled.

#ifdef EACP_HAS_SPIRV
inline void expectStageCompiles(eacp::GPU::Spirv::Stage stage,
                                const std::string& glsl,
                                const std::source_location& location)
{
    const auto result = eacp::GPU::Spirv::compileGlsl(stage, glsl);
    nano::check(result.succeeded(), result.log, location);
}
#endif

inline void expectGlslCompiles(
    const std::string& glsl,
    bool isCompute = false,
    const std::source_location& location = std::source_location::current())
{
#ifdef EACP_HAS_SPIRV
    using eacp::GPU::Spirv::Stage;

    if (isCompute)
    {
        expectStageCompiles(Stage::Compute, glsl, location);
        return;
    }

    expectStageCompiles(Stage::Vertex, glsl, location);
    expectStageCompiles(Stage::Fragment, glsl, location);
#else
    (void) glsl;
    (void) isCompute;
    (void) location;
#endif
}

inline void expectGlslCompiles(
    const eacp::GPU::ShaderGraph& graph,
    const std::source_location& location = std::source_location::current())
{
    expectGlslCompiles(eacp::GPU::emitGlsl(graph), graph.isCompute(), location);
}
