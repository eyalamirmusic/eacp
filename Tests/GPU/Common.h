#pragma once

#include "../TestShaders.h"
#include "CodegenCommon.h"

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/GPU/GPU.h>

#include <string>
#include <string_view>
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
// eacp-spirv wherever that is built, so one that has drifted fails there. The
// WGSL one follows the MSL one, whose conventions WebGPU shares.
struct ShaderTwins
{
    std::string msl;
    std::string hlsl;
    std::string glsl;
    std::string wgsl;
};

// The twins of one shader under Shaders/: "Scissor/green" is
// Scissor/green.metal, .hlsl, .glsl and .wgsl. A dialect without a file is
// empty, as a missing twin always was.
inline ShaderTwins shaderTwins(
    std::string_view name,
    const std::source_location& location = std::source_location::current())
{
    const auto base = std::string {name};
    auto twins = ShaderTwins {testShaderFile(base + ".metal"),
                              testShaderFile(base + ".hlsl"),
                              testShaderFile(base + ".glsl"),
                              testShaderFile(base + ".wgsl")};

    nano::check(!twins.msl.empty() || !twins.hlsl.empty() || !twins.glsl.empty()
                    || !twins.wgsl.empty(),
                "no shader files named " + base,
                location);
    return twins;
}

inline eacp::GPU::ShaderSource nativeDialect(ShaderTwins twins)
{
    return eacp::GPU::ShaderSource::native({std::move(twins.msl),
                                            std::move(twins.hlsl),
                                            std::move(twins.glsl),
                                            std::move(twins.wgsl)});
}

inline eacp::GPU::ShaderSource nativeDialect(
    std::string_view name,
    const std::source_location& location = std::source_location::current())
{
    return nativeDialect(shaderTwins(name, location));
}

inline eacp::GPU::ShaderSource nativeDialect(std::string msl,
                                             std::string hlsl,
                                             std::string glsl,
                                             std::string wgsl)
{
    return nativeDialect(ShaderTwins {
        std::move(msl), std::move(hlsl), std::move(glsl), std::move(wgsl)});
}

inline eacp::GPU::ShaderSource nativeShaderSource(
    ShaderTwins twins,
    const std::source_location& location = std::source_location::current())
{
    expectGlslCompiles(twins.glsl, false, location);
    return nativeDialect(std::move(twins));
}

inline eacp::GPU::ShaderSource nativeShaderSource(
    std::string_view name,
    const std::source_location& location = std::source_location::current())
{
    return nativeShaderSource(shaderTwins(name, location), location);
}

inline eacp::GPU::ShaderSource nativeShaderSource(
    std::string msl,
    std::string hlsl,
    std::string glsl,
    std::string wgsl,
    const std::source_location& location = std::source_location::current())
{
    return nativeShaderSource(
        ShaderTwins {
            std::move(msl), std::move(hlsl), std::move(glsl), std::move(wgsl)},
        location);
}

inline eacp::GPU::ShaderSource nativeComputeShaderSource(
    ShaderTwins twins,
    const std::source_location& location = std::source_location::current())
{
    expectGlslCompiles(twins.glsl, true, location);
    return nativeDialect(std::move(twins));
}

inline eacp::GPU::ShaderSource nativeComputeShaderSource(
    std::string_view name,
    const std::source_location& location = std::source_location::current())
{
    return nativeComputeShaderSource(shaderTwins(name, location), location);
}

inline eacp::GPU::ShaderSource nativeComputeShaderSource(
    std::string msl,
    std::string hlsl,
    std::string glsl,
    std::string wgsl,
    const std::source_location& location = std::source_location::current())
{
    return nativeComputeShaderSource(
        ShaderTwins {
            std::move(msl), std::move(hlsl), std::move(glsl), std::move(wgsl)},
        location);
}

// A vertex-only source: no fragment entry, so only the vertex stage is checked.
inline eacp::GPU::ShaderSource nativeVertexOnlySource(
    ShaderTwins twins,
    const std::source_location& location = std::source_location::current())
{
#ifdef EACP_HAS_SPIRV
    expectStageCompiles(eacp::GPU::Spirv::Stage::Vertex, twins.glsl, location);
#else
    (void) location;
#endif
    auto source = nativeDialect(std::move(twins));
    source.withVertex("vertexMain").withFragment({});
    return source;
}

inline eacp::GPU::ShaderSource nativeVertexOnlySource(
    std::string_view name,
    const std::source_location& location = std::source_location::current())
{
    return nativeVertexOnlySource(shaderTwins(name, location), location);
}
