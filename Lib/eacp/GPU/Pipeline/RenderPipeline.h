#pragma once

#include "../Common.h"

#include "../Texture/Texture.h"
#include "VertexLayout.h"

namespace eacp::GPU
{
class Device;
class ShaderLibrary;

// The colour attachment a pipeline writes; separate from TextureFormat because
// a swapchain drawable is no Texture. Use pixelFormatFor to agree with one.
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

enum class PrimitiveTopology
{
    Triangles,
    TriangleStrip,
    Lines,
    LineStrip,
    Points
};

// AlphaBlend is straight-alpha over (SRC_ALPHA, ONE_MINUS_SRC_ALPHA); Additive
// is alpha-weighted (SRC_ALPHA, ONE).
enum class BlendMode
{
    None,
    AlphaBlend,

    // AlphaBlend's colour with (ONE, ONE_MINUS_SRC_ALPHA) alpha, so the target
    // accumulates coverage rather than squaring it. Identical to AlphaBlend on
    // an opaque target; needed by a texture composited through afterwards.
    AlphaBlendOntoTransparent,

    Additive
};

// Front-facing means counter-clockwise in *clip* space (y up), glTF's
// convention - not either backend's default, which is why both are set
// explicitly. Tests/GPU/CullModeTests.cpp fails if either drifts.
enum class CullMode
{
    None,
    Front,
    Back
};

// Overrides the front-facing convention for geometry that does not arrive in
// it: reversed windings, negative-scale mirrors, inside-out skyboxes.
enum class Winding
{
    CounterClockwise,
    Clockwise
};

// Always with depthWrite off ignores depth without a second pipeline shape;
// Greater is what a reverse-Z projection tests with.
enum class DepthCompare
{
    Never,
    Less,
    LessEqual,
    Equal,
    NotEqual,
    GreaterEqual,
    Greater,
    Always
};

struct RenderPipelineDescriptor
{
    const ShaderLibrary* library = nullptr;
    VertexLayout vertexLayout;
    PixelFormat colorFormat = PixelFormat::BGRA8Unorm;
    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    // Must match the render pass's sample count (GPUView::sampleCount()).
    int sampleCount = 1;
    BlendMode blendMode = BlendMode::None;

    // Requires a depth buffer on the view (GPUView::setDepth(true)); both
    // backends reject a draw whose pipeline disagrees with the pass here.
    bool depth = false;

    // Ignored unless `depth` is set. Translucent geometry wants depthWrite off
    // while still testing, or nearer surfaces hide further ones.
    DepthCompare depthCompare = DepthCompare::LessEqual;
    bool depthWrite = true;

    // frontFace only matters once cullMode is not None.
    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::CounterClockwise;
};

// A compiled pipeline state; create via Device::makeRenderPipeline.
class RenderPipeline
{
public:
    RenderPipeline(Device& device, const RenderPipelineDescriptor& descriptor);

    bool isValid() const;

    PrimitiveTopology topology() const;

    // Read back by the Metal pass, where culling is encoder state rather than
    // part of the pipeline state object it is on D3D12.
    CullMode cullMode() const;
    Winding frontFace() const;

    // nativeDepthState() is null when the pipeline has no depth testing.
    void* nativeState() const;
    void* nativeDepthState() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
