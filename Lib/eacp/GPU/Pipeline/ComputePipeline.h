#pragma once

#include <eacp/Core/Utils/Common.h>

namespace eacp::GPU
{
class Device;
class ShaderLibrary;

// A compiled compute pipeline state (MTLComputePipelineState on Metal). The
// compute sibling of RenderPipeline, built from a library's kernel entry point;
// it carries none of the render fixed-function state. Create via
// Device::makeComputePipeline.
class ComputePipeline
{
public:
    ComputePipeline(Device& device, const ShaderLibrary& library);

    bool isValid() const;

    // Opaque native handle for cross-translation-unit use by the compute pass.
    void* nativeState() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
