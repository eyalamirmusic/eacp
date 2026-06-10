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

// How the vertex stream assembles into primitives. Fixed on the pipeline
// (D3D-style); the Metal backend reads it off the pipeline at draw time.
enum class PrimitiveTopology
{
    Triangles,
    TriangleStrip,
    Lines,
    LineStrip,
    Points
};

struct RenderPipelineDescriptor
{
    const ShaderLibrary* library = nullptr;
    VertexLayout vertexLayout;
    PixelFormat colorFormat = PixelFormat::BGRA8Unorm;
    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    // Multisample count for anti-aliasing. Must match the render pass's sample
    // count (GPUView::sampleCount()). 1 = no MSAA.
    int sampleCount = 1;
    bool blending = false;

    // Depth testing (less-equal, depth writes on). Requires the view to provide a
    // depth buffer (GPUView::setDepth(true)). Needed for correct 3D occlusion.
    bool depth = false;
};

// A compiled render pipeline state (MTLRenderPipelineState on Metal). Create via
// Device::makeRenderPipeline.
class RenderPipeline
{
public:
    RenderPipeline(Device& device, const RenderPipelineDescriptor& descriptor);

    bool isValid() const;

    // The descriptor's topology, read back by the render pass at draw time.
    PrimitiveTopology topology() const;

    // Opaque native handles for cross-translation-unit use by the render pass.
    // nativeDepthState() is null when the pipeline has no depth testing.
    void* nativeState() const;
    void* nativeDepthState() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
