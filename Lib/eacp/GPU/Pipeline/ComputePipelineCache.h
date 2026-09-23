#pragma once

#include "../Shader/ShaderLibrary.h"
#include "../Shader/ShaderSource.h"
#include "ComputePipeline.h"

#include <memory>

namespace eacp::GPU
{
class Device;

// A compute source compiled on one Device: the library and the pipeline built
// from it, held together because the pipeline was made from that library.
struct CompiledCompute
{
    CompiledCompute(Device& device, const ShaderSource& source);

    ShaderLibrary library;
    ComputePipeline pipeline;
};

// The compiled form of a compute source on a Device, built the first time that
// source is asked for and shared from then on, for as long as the Device
// lives. Two sources are the same when everything the backend compiles from is
// - backend, entry point, thread group, bindings and text - so a kernel
// constructed a thousand times pays the shader compiler once: MSL through
// newLibraryWithSource and a pipeline state on Metal, DXC and
// CreateComputePipelineState on D3D12, glslang and a VkPipeline on Vulkan.
//
// Safe from any thread. Two threads asking for one source at once compile it
// once, the second waiting for the first.
std::shared_ptr<const CompiledCompute>
    compileComputeCached(Device& device, const ShaderSource& source);
} // namespace eacp::GPU
