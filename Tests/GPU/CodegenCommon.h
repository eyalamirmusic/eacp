#pragma once

// What the device-free suites need and nothing more: the EDSL, the two
// emitters, and the pass headers the binding constants live in. No Device, no
// GPUView, no window — which is what lets GPUCodegenTests link
// eacp-gpu-codegen alone and run on a host with no GPU and no display.
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

// Every GLSL string a test emits is also handed to glslang, so the dialect has
// to be a program and not just the right sequence of characters. String
// assertions pin what the emitter says; this pins that a compiler accepts it,
// on macOS and Windows as much as on the platform that will run it.
//
// eacp-spirv is optional (EACP_BUILD_SPIRV), so without it these compile away
// and the suite still builds and passes on its string assertions alone.

#ifdef EACP_HAS_SPIRV
inline void expectStageCompiles(eacp::GPU::Spirv::Stage stage,
                                const std::string& glsl,
                                const std::source_location& location)
{
    const auto result = eacp::GPU::Spirv::compileGlsl(stage, glsl);
    nano::check(result.succeeded(), result.log, location);
}
#endif

// A render source compiles as both stages out of the one string; a kernel has
// the single unguarded main().
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
