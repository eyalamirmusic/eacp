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

    // AlphaBlend in the colour channels and (ONE, ONE_MINUS_SRC_ALPHA) in the
    // alpha one, which is what a target whose own alpha will be read back needs.
    //
    // The difference is invisible on a drawable and decisive on a texture.
    // AlphaBlend weights the source's alpha by itself, so a fragment at 50%
    // coverage over nothing leaves 25% alpha behind rather than 50%: on an
    // opaque surface nobody looks at that channel, and on a texture something is
    // about to composite through it, and every antialiased edge in it comes out
    // too transparent. This accumulates coverage the way the colour channels
    // accumulate colour, so a texture rendered through it holds exactly the
    // coverage that was drawn into it.
    //
    // Which makes it the mode to render a *layer* through -- a subtree drawn
    // into a texture so it can be faded, masked or filtered as a unit -- and
    // costs nothing to use on an opaque target, where the destination alpha is 1
    // and both modes leave it 1.
    AlphaBlendOntoTransparent,

    Additive
};

// Which faces the rasterizer keeps. None draws both and is the default: it is
// where both backends start, and it is what a mesh whose winding is not known
// to be consistent needs - a wrongly-wound triangle under culling does not draw
// wrongly, it does not draw at all.
//
// The default winding convention is **a triangle whose vertices run
// counter-clockwise in clip space - the space setPosition writes, with y up -
// is front-facing.** That is glTF's convention, and it is the one worth stating
// in the space a shader is written in rather than in the image, where the
// viewport's y flip has already reversed the sign of it.
//
// Worth stating because both backends default to "clockwise is front", which is
// the opposite of it. They do agree on what winding means, though: clip-space y
// is up and the framebuffer origin is top left on each, so the same convention
// is spelled the same way on both - MTLWindingCounterClockwise on Metal,
// FrontCounterClockwise = TRUE on D3D12, both explicitly, with
// Tests/GPU/CullModeTests.cpp there to fail if either drifts.
enum class CullMode
{
    None,
    Front,
    Back
};

// Which winding counts as the front face, in the clip space CullMode's note
// states the convention in.
//
// CounterClockwise is the default and is that convention, so a pipeline that
// says nothing about winding gets glTF's answer. The field exists for the
// geometry that does not arrive in it: a mesh wound the other way, an instance
// mirrored by a negative scale - which reverses the winding of every triangle
// in it - or an inside-out shape like a skybox, none of which should need its
// indices rewritten to draw.
enum class Winding
{
    CounterClockwise,
    Clockwise
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

    // Face culling, off by default. Worth turning on for closed geometry whose
    // winding is consistent: a back face is rasterised and shaded before the
    // depth test throws it away, so culling it is work not done rather than work
    // undone.
    //
    // frontFace only matters once cullMode is not None, and its default is the
    // convention CullMode's note states.
    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::CounterClockwise;
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

    // The descriptor's cull mode and front face. Metal reads them here and sets
    // them on the encoder, face culling being encoder state there rather than
    // part of the pipeline state object it is on D3D12.
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
