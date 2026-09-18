#pragma once

// Device-free, so GPUCodegenTests links eacp-gpu-codegen alone.
#include <eacp/Core/Platform/Platform.h>
#include <eacp/GPU/Codegen/GlslLowering.h>
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

// Where eacp-spirv is built (Linux by default) the emitted GLSL is compiled -
// for Vulkan as it stands, and for OpenGL through the lowering pass.

#ifdef EACP_HAS_SPIRV
inline eacp::GPU::ShaderStage toShaderStage(eacp::GPU::Spirv::Stage stage)
{
    using eacp::GPU::ShaderStage;

    switch (stage)
    {
        case eacp::GPU::Spirv::Stage::Fragment:
            return ShaderStage::Fragment;
        case eacp::GPU::Spirv::Stage::Compute:
            return ShaderStage::Compute;
        case eacp::GPU::Spirv::Stage::Vertex:
            break;
    }

    return ShaderStage::Vertex;
}

// The floor and the compute tier of each profile: what a GL backend meets.
inline eacp::Vector<eacp::GPU::GlslTarget> glslTargets()
{
    using eacp::GPU::GlslTarget;
    using Profile = GlslTarget::Profile;

    return {{Profile::Core, 330},
            {Profile::Core, 430},
            {Profile::ES, 300},
            {Profile::ES, 310}};
}

// A lowering regression fails here rather than as a wrong pixel on the one lane
// with a GL device: every source the suite compiles for Vulkan is lowered for
// the four targets and validated against glslang's OpenGL client. A source the
// target is too old for - a kernel on GL 3.3 - is what needsNewerTarget says,
// and is not a failure.
inline void expectLowersForOpenGL(eacp::GPU::Spirv::Stage stage,
                                  const std::string& glsl,
                                  const std::source_location& location)
{
    auto validated = 0;

    for (const auto& target: glslTargets())
    {
        const auto lowered =
            eacp::GPU::lowerGlsl(glsl, target, toShaderStage(stage));

        if (lowered.needsNewerTarget)
            continue;

        nano::check(lowered.succeeded(), lowered.error, location);

        if (!lowered.succeeded())
            continue;

        const auto result =
            eacp::GPU::Spirv::validateGlsl(stage, lowered.source, target);

        nano::check(result.succeeded, result.log, location);
        ++validated;
    }

    // The compute tier of each profile carries everything the emitter writes,
    // so a source that reached no target at all is the pass refusing them.
    nano::check(validated > 0, "no GL target carried this source", location);
}

inline void expectStageCompiles(eacp::GPU::Spirv::Stage stage,
                                const std::string& glsl,
                                const std::source_location& location)
{
    const auto result = eacp::GPU::Spirv::compileGlsl(stage, glsl);
    nano::check(result.succeeded(), result.log, location);

    expectLowersForOpenGL(stage, glsl, location);
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
