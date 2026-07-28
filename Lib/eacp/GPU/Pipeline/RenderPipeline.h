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

// Which faces the rasterizer discards before shading them. None draws both,
// which is what a 2D renderer wants and what both backends did before this
// existed. Back is the setting for closed geometry - a mesh's far side is
// hidden by its near side anyway, so shading it is work thrown away, and glTF
// models are authored expecting it.
enum class CullMode
{
    None,
    Back,
    Front
};

// Which winding, seen on the rendered image, counts as the front face.
//
// Clockwise is the default because it is what both backends already did:
// Metal's own default is MTLWindingClockwise and D3D12's is
// FrontCounterClockwise = FALSE. glTF defines its front faces the other way -
// counter-clockwise in *its* coordinates - so a loader picks the value that
// matches after its own handedness flip rather than assuming either.
enum class Winding
{
    Clockwise,
    CounterClockwise
};

// The test a fragment's depth has to pass against what is already in the depth
// buffer to be kept. Less-equal is the conventional choice for a near-to-far
// depth buffer cleared to 1, and is what this had baked in before it was a
// choice.
//
// The two that look redundant are not. Always with depthWrite off is how a
// pass draws while ignoring depth entirely without needing a second pipeline
// shape, and Greater is what a reverse-Z projection - the standard fix for
// depth precision at distance - tests with.
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
    // Multisample count for anti-aliasing. Must match the render pass's sample
    // count (GPUView::sampleCount()). 1 = no MSAA.
    int sampleCount = 1;
    BlendMode blendMode = BlendMode::None;

    // Whether this pipeline has a depth attachment at all. Requires the view to
    // provide a depth buffer (GPUView::setDepth(true)), and both backends reject
    // a draw whose pipeline disagrees with the pass about whether one is there -
    // so this is the attachment, and the two fields below are what to do with it.
    bool depth = false;

    // How the depth test is run. Ignored unless `depth` is set, since without an
    // attachment there is nothing to compare against.
    //
    // depthWrite is separate from the comparison because the two come apart
    // exactly where it matters: translucent geometry has to test against the
    // opaque depth already there and must *not* write, or the nearer of two
    // translucent surfaces hides the further one instead of blending over it.
    DepthCompare depthCompare = DepthCompare::LessEqual;
    bool depthWrite = true;

    // Face culling. Off by default: a 2D renderer's quads have no meaningful
    // winding, and turning this on by default would make half of them vanish.
    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::Clockwise;
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

    // Culling, read back by the render pass for the same reason topology is:
    // Metal sets it on the encoder rather than baking it into the pipeline, so
    // the pass has to apply it when the pipeline is bound. On D3D12 both are
    // already in the PSO and these are only here so the two backends present the
    // same class.
    CullMode cullMode() const;
    Winding frontFace() const;

    // Opaque native handles for cross-translation-unit use by the render pass.
    // nativeDepthState() is null when the pipeline has no depth testing.
    void* nativeState() const;
    void* nativeDepthState() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
