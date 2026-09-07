#pragma once

#include "CodegenCommon.h"

#include <eacp/GPU/GPU.h>

#include <string>
#include <utility>

// The hand-written twins, one per dialect. The GLSL one also goes through
// eacp-spirv wherever that is built, so one that has drifted fails there.
inline eacp::GPU::ShaderSource
    nativeDialect(std::string msl, std::string hlsl, std::string glsl)
{
    if constexpr (eacp::Platform::isWindows())
        return eacp::GPU::ShaderSource::hlsl(std::move(hlsl));
    else if constexpr (eacp::Platform::isLinux())
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
