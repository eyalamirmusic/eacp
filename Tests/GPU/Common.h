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

// Every case self-skips on Device::isValid(); a case that dispatches a kernel
// self-skips on this instead, which is the same question one tier down - a
// device whose backend has no compute at all (plan.md D12). The OpenGL backend
// is the one that answers false today, and a GL context below 4.3 will answer
// false once its tier is built.
inline bool computeIsAvailable()
{
    auto& device = eacp::GPU::Device::shared();

    return device.isValid() && device.supportsCompute();
}

// The two capability skips beside computeIsAvailable, and the same shape: a
// question about the device, asked before the case builds anything that depends
// on the answer.
//
// Storage buffers are GL 4.3 / ES 3.1, so a case that binds one - or that
// builds a shader naming one - self-skips below that. Every other backend
// answers yes.
inline bool storageBuffersAreAvailable()
{
    auto& device = eacp::GPU::Device::shared();

    return device.isValid() && device.supportsStorageBuffers();
}

// A case that reads an absolute depth value back self-skips where clip space
// leaves depth in [-1, 1] rather than [0, 1] - a GL context with no
// glClipControl, which is what a virtualised driver commonly is. Ordering is
// unaffected there, so the depth *tests* beside these still run.
inline bool zeroToOneDepthIsAvailable()
{
    auto& device = eacp::GPU::Device::shared();

    return device.isValid() && device.supportsZeroToOneDepth();
}

// Whether this Device is one built out of two backends - Linux's composite,
// OpenGL for render and Vulkan for compute (plan.md D11) - where a resource
// both halves touch exists twice and what one wrote is copied through host
// memory before the other reads it. Every other device crosses nothing, so
// Device::crossingBytesThisFrame() stays at zero there, which is what the
// crossing cases assert on it.
inline bool deviceCrossesResources()
{
    return eacp::GPU::Device::shared().backendName() == "OpenGL+Vulkan";
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
