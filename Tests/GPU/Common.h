#pragma once

#include "CodegenCommon.h"

#include <eacp/GPU/GPU.h>

#include <string>
#include <utility>

// The hand-written shader twins, one per dialect, picked by the platform whose
// compiler is about to see it.
//
// The GLSL twin is the one no device here can run - there is no Vulkan backend
// yet - so it is compiled through eacp-spirv on the way past instead. That is
// what keeps it honest: a twin that has drifted from the MSL and HLSL beside it
// fails on macOS and Windows CI rather than on the day the backend lands.
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

// The same for a kernel, whose single main() carries no stage macro and so
// compiles as one stage rather than two. The caller still names the entry point
// with withCompute(), which is what marks the source as compute.
inline eacp::GPU::ShaderSource nativeComputeShaderSource(
    std::string msl,
    std::string hlsl,
    std::string glsl,
    const std::source_location& location = std::source_location::current())
{
    expectGlslCompiles(glsl, true, location);
    return nativeDialect(std::move(msl), std::move(hlsl), std::move(glsl));
}
