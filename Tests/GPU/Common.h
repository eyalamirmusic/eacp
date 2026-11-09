#pragma once

#include "CodegenCommon.h"

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/GPU/GPU.h>

#include <string>
#include <utility>

// WARP's ARM64 JIT access-violates running the three largest kernels in the
// suite - TypesDevice's 4x4 matrix kernel, Float16Device's builtins and its
// four-column matrices - from FXC's DXBC and DXC's DXIL alike, at a PC in no
// loaded module, while every smaller kernel runs and the same DXBC runs on
// x64 WARP and on a hardware adapter. Those cases keep their CPU halves on
// that machine and leave the GPU one out (CrossCheck::cpuOnlyWhen).
// EACP_TEST_WARP_ARM64=1 runs them anyway, to see whether a newer WARP
// still crashes.
inline bool isWarpOnArm64()
{
    if (eacp::getEnvValue("EACP_TEST_WARP_ARM64") == "1")
        return false;

    return eacp::Platform::architectureName() == "arm64"
           && eacp::GPU::Device::shared().name().find("Basic Render Driver")
                  != std::string::npos;
}

// The hand-written twins, one per dialect. The GLSL one also goes through
// eacp-spirv wherever that is built, so one that has drifted fails there.
inline eacp::GPU::ShaderSource
    nativeDialect(std::string msl, std::string hlsl, std::string glsl)
{
    if constexpr (eacp::Platform::isWindows())
        return eacp::GPU::ShaderSource::hlsl(std::move(hlsl));
    else if constexpr (eacp::Platform::isLinuxFamily())
        return eacp::GPU::ShaderSource::glsl(std::move(glsl));
    else
        return eacp::GPU::ShaderSource::msl(std::move(msl));
}

inline eacp::GPU::ShaderSource nativeShaderSource(
    std::string msl,
    std::string hlsl,
    std::string glsl,
    const std::source_location& location = std::source_location::current())
{
    expectGlslCompiles(glsl, false, location);
    return nativeDialect(std::move(msl), std::move(hlsl), std::move(glsl));
}

inline eacp::GPU::ShaderSource nativeComputeShaderSource(
    std::string msl,
    std::string hlsl,
    std::string glsl,
    const std::source_location& location = std::source_location::current())
{
    expectGlslCompiles(glsl, true, location);
    return nativeDialect(std::move(msl), std::move(hlsl), std::move(glsl));
}

// A vertex-only source: no fragment entry, so only the vertex stage is checked.
inline eacp::GPU::ShaderSource nativeVertexOnlySource(
    std::string msl,
    std::string hlsl,
    std::string glsl,
    const std::source_location& location = std::source_location::current())
{
#ifdef EACP_HAS_SPIRV
    expectStageCompiles(eacp::GPU::Spirv::Stage::Vertex, glsl, location);
#else
    (void) location;
#endif
    auto source = nativeDialect(std::move(msl), std::move(hlsl), std::move(glsl));
    source.withVertex("vertexMain").withFragment({});
    return source;
}
