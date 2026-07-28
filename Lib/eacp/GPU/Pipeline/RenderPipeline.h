#pragma once

#include "../Common.h"

#include "../Texture/Texture.h"
#include "VertexLayout.h"

namespace eacp::GPU
{
class Device;
class ShaderLibrary;

// The colour attachment a pipeline writes. Separate from TextureFormat because
// a pipeline usually targets a swapchain drawable, which no Texture stands for
// - but one that renders into a texture has to agree with it, which is what
// pixelFormatFor is for.
enum class PixelFormat
{
    BGRA8Unorm,
    RGBA8Unorm,
    RGBA16Float,
    RGBA32Float
};

constexpr PixelFormat pixelFormatFor(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::BGRA8Unorm:
            return PixelFormat::BGRA8Unorm;
        case TextureFormat::RGBA16Float:
            return PixelFormat::RGBA16Float;
        case TextureFormat::RGBA32Float:
            return PixelFormat::RGBA32Float;
        default:
            return PixelFormat::RGBA8Unorm;
    }
}

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

// Which faces the rasterizer keeps. None draws both and is the default: it is
// where both backends start, and it is what a mesh whose winding is not known
// to be consistent needs - a wrongly-wound triangle under culling does not draw
// wrongly, it does not draw at all.
//
// The winding convention is stated once, here, because it is the half of face
// culling the backends do not agree on by themselves: **a triangle whose
// vertices run counter-clockwise in clip space - the space setPosition writes,
// with y up - is front-facing.** That is glTF's convention, and it is the one
// worth stating in the space a shader is written in rather than in the image,
// where the viewport's y flip has already reversed it.
//
// Both defaults read "clockwise is front" and mean different things by it:
// Metal decides facing in clip space and D3D12 in screen space, one y flip
// apart, so left to themselves the two would cull opposite faces of the same
// mesh. Metal is therefore set to MTLWindingCounterClockwise and D3D12 keeps
// FrontCounterClockwise = FALSE, both explicitly, and
// Tests/GPU/CullModeTests.cpp is what fails if either drifts.
enum class CullMode
{
    None,
    Front,
    Back
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

    // Depth testing (less-equal, depth writes on). Requires the view to provide a
    // depth buffer (GPUView::setDepth(true)). Needed for correct 3D occlusion.
    bool depth = false;

    // Face culling, off by default. Worth turning on for closed geometry whose
    // winding is consistent: a back face is rasterised and shaded before the
    // depth test throws it away, so culling it is work not done rather than work
    // undone.
    CullMode cullMode = CullMode::None;
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

    // The descriptor's cull mode. Metal reads it here and sets it on the
    // encoder, face culling being encoder state there rather than part of the
    // pipeline state object it is on D3D12.
    CullMode cullMode() const;

    // Opaque native handles for cross-translation-unit use by the render pass.
    // nativeDepthState() is null when the pipeline has no depth testing.
    void* nativeState() const;
    void* nativeDepthState() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
