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

// Color-attachment blending. AlphaBlend is the classic straight-alpha over
// (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), for translucency stacking. Additive is
// alpha-weighted (SRC_ALPHA, ONE), for glow / accumulation effects where
// overlapping fragments brighten. None keeps the pipeline opaque - the source
// fragment replaces whatever was in the target.
enum class BlendMode
{
    None,
    AlphaBlend,
    Additive
};

// Back-compat wrapper for the old `bool blending` field. Marking a bool
// member `[[deprecated]]` directly would fire the warning inside the
// descriptor's implicit copy/move constructors too, spraying deprecation
// noise across every caller that returns a descriptor by value. Scoping the
// attribute to the assignment operator instead means only the write
// (`descriptor.blending = true;`) triggers the warning, while reads and
// copies stay silent. Read implicitly converts back to bool so backends and
// resolveBlendMode() see it as a plain bool.
struct DeprecatedBlendingField
{
    bool value = false;

    DeprecatedBlendingField() = default;

    [[deprecated("Use blendMode = BlendMode::AlphaBlend instead.")]]
    DeprecatedBlendingField& operator=(bool newValue)
    {
        value = newValue;
        return *this;
    }

    operator bool() const { return value; }
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
    BlendMode blendMode = BlendMode::None;

    // Deprecated: set `blendMode = BlendMode::AlphaBlend` instead. Pre-BlendMode
    // code (`descriptor.blending = true;`) still compiles and behaves the same.
    // Resolved by resolveBlendMode() below: if blendMode is anything but None
    // it wins; only then does `blending` upgrade a None to AlphaBlend.
    DeprecatedBlendingField blending;

    // Depth testing (less-equal, depth writes on). Requires the view to provide a
    // depth buffer (GPUView::setDepth(true)). Needed for correct 3D occlusion.
    bool depth = false;
};

// Resolves the effective blend mode from the descriptor, honouring the
// deprecated `blending` bool for callers that haven't migrated yet. `blendMode`
// wins whenever set; the bool only upgrades a None default to AlphaBlend.
inline BlendMode resolveBlendMode(const RenderPipelineDescriptor& descriptor)
{
    if (descriptor.blendMode != BlendMode::None)
        return descriptor.blendMode;

    return descriptor.blending ? BlendMode::AlphaBlend : BlendMode::None;
}

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
