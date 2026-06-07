#pragma once

#include <eacp/Core/Utils/Common.h>

#include "VertexLayout.h"

namespace eacp::GPU
{
class Device;
class ShaderLibrary;

enum class PixelFormat
{
    BGRA8Unorm
};

struct RenderPipelineDescriptor
{
    const ShaderLibrary* library = nullptr;
    VertexLayout vertexLayout;
    PixelFormat colorFormat = PixelFormat::BGRA8Unorm;
    bool blending = false;
};

// A compiled render pipeline state (MTLRenderPipelineState on Metal). Create via
// Device::makeRenderPipeline.
class RenderPipeline
{
public:
    RenderPipeline(Device& device, const RenderPipelineDescriptor& descriptor);

    bool isValid() const;

    // Opaque native handle for cross-translation-unit use by the render pass.
    void* nativeState() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
