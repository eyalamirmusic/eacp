#pragma once

#include "../Common.h"

#include "../Texture/Texture.h"
#include "VertexLayout.h"

#include <optional>

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
    RGBA32Float,
    R32Float,

    // Every renderable TextureFormat (isRenderableFormat: all but the block
    // formats and RGB9E5Float) has its attachment format. The integer
    // ones - R32Uint and the Uint and Sint members below - are integer
    // attachments: the fragment function returns a uint or an int of the
    // target's width, and they take no blending. The sRGB ones encode what
    // the fragment returns, and blend in linear light before they do.
    R8Unorm,
    RG8Unorm,
    R16Float,
    RG16Float,
    RG32Float,
    R16Unorm,
    R32Uint,
    RGBA8UnormSrgb,
    BGRA8UnormSrgb,
    RG16Unorm,
    RGBA16Unorm,
    RGB10A2Unorm,
    RG11B10Float,
    R8Uint,
    RG8Uint,
    RGBA8Uint,
    R16Uint,
    RG16Uint,
    RGBA16Uint,
    RG32Uint,
    RGBA32Uint,
    R8Sint,
    RG8Sint,
    RGBA8Sint,
    R16Sint,
    RG16Sint,
    RGBA16Sint,
    R32Sint,
    RG32Sint,
    RGBA32Sint
};

// Exhaustive rather than defaulted, so that a texture format added without an
// attachment format is a -Wswitch warning here rather than a pipeline silently
// built against RGBA8. The formats that fall through are the block-compressed
// ones and RGB9E5Float, which cannot be a render target at all.
PixelFormat pixelFormatFor(TextureFormat format);

// The way back: the texture format an attachment format is the attachment of.
// Total, every PixelFormat being a renderable TextureFormat, which is what
// lets each backend map one table of native formats rather than two.
TextureFormat textureFormatFor(PixelFormat format);

// Whether a pipeline writing this attachment returns integers from its
// fragment function, and so can take no blending.
bool isIntegerFormat(PixelFormat format);

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

// One operand's weight in the blend equation:
//
//     result = source * sourceFactor  <op>  destination * destinationFactor
//
// The same eleven values on both backends, because MTLBlendFactor and
// D3D12_BLEND took them from the same place. `Source` is the fragment the
// shader just produced; `Destination` is what is already in the attachment.
//
// This is the escape hatch, not the front door. BlendMode's four presets are
// what a UI, a sprite or a glyph wants, and they are named because those are
// the four that come up. What needs the factors themselves is content whose
// *author* chose the equation - a material system, where "modulate by what is
// behind me" and "weight by the destination's alpha" are things written in a
// file that the renderer has to honour rather than approximate.
enum class BlendFactor
{
    Zero,
    One,
    SourceColor,
    OneMinusSourceColor,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationColor,
    OneMinusDestinationColor,
    DestinationAlpha,
    OneMinusDestinationAlpha,

    // min(sourceAlpha, 1 - destinationAlpha) in all four channels. The one
    // factor that is not a plain operand.
    SourceAlphaSaturated
};

// How the two weighted operands are combined. Add is the equation above; the
// other four are what both APIs offer beside it, spelled the same way.
//
// Subtract and ReverseSubtract differ only in which side is taken from which,
// and the names follow the convention OpenGL and D3D share: Subtract is source
// minus destination.
//
// Min and Max ignore the factors entirely on both backends - they compare the
// unweighted operands - which is worth knowing before setting factors that
// then do nothing.
enum class BlendOperation
{
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max
};

// The whole blend equation, colour and alpha separately, for a caller that
// needs a combination BlendMode does not name.
//
// The two halves are separate because both APIs separate them, and because the
// difference is decisive on a texture that will be composited later - which is
// the entire argument for BlendMode::AlphaBlendOntoTransparent, expressible
// here as the pair of equations it actually is.
//
// The defaults are BlendMode::None: the source replaces the destination. The
// factors are written as (One, Zero) rather than left arbitrary so that turning
// `enabled` on by itself means exactly the same thing.
//
// **One asymmetry between the backends is closed here rather than passed on.**
// D3D12 rejects the four `*Color` factors in the alpha slots outright - a
// pipeline naming SourceColor for `sourceAlpha` fails to create - while Metal
// accepts them. It is not a capability difference: in the alpha channel, the
// alpha component of SourceColor *is* SourceAlpha, so the two express the same
// arithmetic and D3D12 simply refuses the redundant spelling. The Windows
// backend therefore substitutes the alpha equivalent in those two fields, and
// both backends compute the same number for the same BlendState. Stated because
// a reader comparing the two files will otherwise see a translation that looks
// wrong.
struct BlendState
{
    bool enabled = false;

    BlendFactor sourceColor = BlendFactor::One;
    BlendFactor destinationColor = BlendFactor::Zero;
    BlendOperation colorOperation = BlendOperation::Add;

    BlendFactor sourceAlpha = BlendFactor::One;
    BlendFactor destinationAlpha = BlendFactor::Zero;
    BlendOperation alphaOperation = BlendOperation::Add;
};

// What each named mode is, written out once. Both backends build their state
// through this rather than each switching on BlendMode themselves, so a
// preset's equation is stated in one place and the two cannot drift on what a
// preset means.
BlendState blendStateFor(BlendMode mode);

// Which channels a fragment is allowed to write. Every channel by default,
// which is where both backends start.
//
// What this is for is a pass that has to update the depth or the stencil plane
// while leaving the picture alone - a shadow volume being counted, a depth
// prepass - and the reason it is a field rather than a workaround is that the
// workaround only covers one case of it. Drawing additively at zero alpha
// leaves the destination untouched, which is "writes nothing"; there is no
// trick at all for "writes green but not red", which a material system's
// maskRed / maskColor / maskAlpha keywords ask for directly.
//
// Independent of the blend equation: a masked channel is not written whatever
// the blend computed for it.
struct ColorWriteMask
{
    bool red = true;
    bool green = true;
    bool blue = true;
    bool alpha = true;

    // Nothing at all - the case the additive-at-zero-alpha workaround was
    // standing in for.
    static ColorWriteMask none();
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

// The test a fragment has to pass to be kept, against what is already in the
// depth buffer or - for a stencil face - against the pass's reference value.
// Less-equal is the conventional depth choice for a near-to-far buffer cleared
// to 1, and is what this had baked in before it was a choice.
//
// The two that look redundant are not. Always with depthWrite off is how a
// pass draws while ignoring depth entirely without needing a second pipeline
// shape, and Greater is what a reverse-Z projection - the standard fix for
// depth precision at distance - tests with.
//
// One enum for both tests because both APIs have one: MTLCompareFunction and
// D3D12_COMPARISON_FUNC are each used for depth and stencil alike, so a second
// enum would be the same eight values under another name and one more place for
// the two backends to disagree.
enum class CompareFunction
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

// The name this was born under, kept because `depthCompare` reads better with
// it and nothing is served by rewriting every call site.
using DepthCompare = CompareFunction;

// The polygon offset added to every fragment's depth before the test and the
// write:
//
//     bias = constant * r + slopeScale * max(|dz/dx|, |dz/dy|)
//
// clamped to at most `clamp` when that is positive and at least it when it is
// negative; zero clamps nothing. `r` is the smallest representable difference
// in depth at the primitive's depth - the depth plane is 32-bit float on every
// backend, so 2^(e - 23) with `e` the exponent of the primitive's largest z,
// and one unit near z = 0.5 is about 6e-8. The slope is depth per pixel.
//
// **A positive bias makes depth larger**, which is further from the camera
// under the conventional LessEqual buffer cleared to 1 and *nearer* under a
// reversed-Z one cleared to 0 and tested GreaterEqual - so a shadow caster
// pushed away from the light takes a positive bias in the first and a negative
// one in the second. Tests/GPU/DepthBiasTests.cpp measures it on each backend.
//
// The three are the same three on every API - D3D12's rasterizer desc, Vulkan's
// rasterization state, Metal's encoder - with one difference: D3D12 takes the
// constant as an integer, so it is rounded to the nearest whole unit there, and
// a whole-unit constant is the portable one.
struct DepthBias
{
    float constant = 0.f;
    float slopeScale = 0.f;
    float clamp = 0.f;

    bool isNone() const;
};

// What happens to the stencil value in the buffer when a fragment reaches one
// of the three outcomes a StencilFace names.
//
// IncrementWrap and DecrementWrap are the pair that matter for shadow volumes:
// the count of front faces entered minus back faces left is what a stencil
// shadow accumulates, and the clamping forms lose it the moment a pixel is
// inside more shadow volumes than the buffer's 255. Wrapping is what keeps the
// arithmetic modular, so an over- and an under-count still cancel.
enum class StencilOp
{
    Keep,
    Zero,
    Replace,
    IncrementClamp,
    DecrementClamp,
    Invert,
    IncrementWrap,
    DecrementWrap
};

// What one facing does with the stencil buffer: the test it runs against the
// pass's reference value, and what it writes on each of the three outcomes.
//
// Front and back are separate because the one algorithm that most needs stencil
// needs them to differ: a two-sided depth-fail shadow volume increments on the
// back faces and decrements on the front ones in a single pass over the
// geometry, which is what glStencilOpSeparate exists for and what a
// one-face-at-a-time API costs two passes.
//
// The defaults are the pass-through: always test, never write. A pipeline that
// sets `stencil` and leaves a face alone therefore reads the buffer without
// disturbing it, which is what the shading pass of a shadowed scene wants for
// both faces once the volume pass has filled it in.
struct StencilFace
{
    CompareFunction compare = CompareFunction::Always;

    // Applied when the stencil test fails, so the fragment is discarded.
    StencilOp stencilFail = StencilOp::Keep;

    // Applied when the stencil test passes and the depth test then fails. This
    // is the outcome the depth-fail (Carmack's reverse) shadow algorithm counts,
    // and it is the reason a stencil pipeline usually wants a depth buffer too.
    StencilOp depthFail = StencilOp::Keep;

    // Applied when both tests pass and the fragment is kept.
    StencilOp pass = StencilOp::Keep;
};

// What a pipeline writes to one colour attachment of a pass over several
// targets: the attachment's format, its blend and its write mask, each meaning
// what the field of the same name on RenderPipelineDescriptor means for
// attachment 0. Every attachment blends independently.
struct ColorTargetState
{
    PixelFormat format = PixelFormat::RGBA8Unorm;
    BlendMode blendMode = BlendMode::None;
    std::optional<BlendState> blend;
    ColorWriteMask writeMask;
};

struct RenderPipelineDescriptor
{
    const ShaderLibrary* library = nullptr;
    VertexLayout vertexLayout;
    PixelFormat colorFormat = PixelFormat::BGRA8Unorm;
    PrimitiveTopology topology = PrimitiveTopology::Triangles;

    int sampleCount = 1;
    BlendMode blendMode = BlendMode::None;

    // The blend equation in full, when one of the four named modes is not it.
    //
    // Unset - which it is by default - and blendMode decides, so nothing that
    // has ever been written against this struct changes. Set, and it wins:
    // there is no merging of the two, because a caller who has written out an
    // equation has said what they mean and reading blendMode as well would only
    // create a way to disagree with it.
    std::optional<BlendState> blend;

    // Which channels reach the attachment, after the blend.
    ColorWriteMask colorWriteMask;

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

    // Pushes this pipeline's depth along the buffer, which is what keeps a
    // shadow map from shadowing the surface it was rendered from. None by
    // default, and ignored unless `depth` is set. See DepthBias.
    DepthBias depthBias;

    // Face culling, off by default. Worth turning on for closed geometry whose
    // winding is consistent: a back face is rasterised and shaded before the
    // depth test throws it away, so culling it is work not done rather than work
    // undone.
    //
    // frontFace only matters once cullMode is not None, and its default is the
    // convention CullMode's note states.
    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::CounterClockwise;

    // Whether this pipeline's attachment carries a stencil plane, and so
    // whether the two faces below are read at all.
    //
    // It is the same question `depth` asks and has the same answer: the
    // attachment's format is part of what a pipeline is compiled against, so
    // this must agree with the pass it draws into or the draw is rejected.
    // Requires GPUView::setStencil(true), or TextureDescriptor::stencil on a
    // texture target - each of which also gives the pass a depth buffer, the
    // two planes being one attachment on both APIs.
    //
    // Setting this does not enable the depth test: `depth` still does that, and
    // a pipeline may reasonably set stencil with depth off (a mask being
    // painted) or both together (a shadow volume, which counts depth failures).
    //
    // It does, though, have to be set by *every* pipeline drawing into a pass
    // whose attachment carries a stencil plane, including one that only wants
    // the depth test - the format of the attachment is what a pipeline is
    // compiled against, and a stencilled buffer is a different format from a
    // plain depth one. A view left on setStencil(false) is the cheaper
    // attachment and the one to keep where nothing needs the plane.
    bool stencil = false;

    StencilFace stencilFront;
    StencilFace stencilBack;

    // Which bits a stencil comparison reads, and which bits a stencil write may
    // touch. Both default to every bit.
    //
    // One pair for both faces rather than a pair on each, which is the narrower
    // of the two APIs: D3D12 carries a single StencilReadMask and
    // StencilWriteMask on the whole depth-stencil state, while Metal has them
    // per face. Offering per-face masks would mean offering something one
    // backend cannot honour, so the shared pair is what both can mean exactly.
    // No algorithm this exists for wants them to differ: a write mask splits the
    // buffer into bitfields, and the split is a property of the buffer rather
    // than of a facing.
    unsigned char stencilReadMask = 0xff;
    unsigned char stencilWriteMask = 0xff;

    // Attachments 1 and up, for a pipeline drawing into a pass over several
    // targets (Frame::beginMultiTargetPass). Attachment 0 is colorFormat,
    // blendMode, blend and colorWriteMask above, so a pipeline drawing into one
    // target leaves this empty and means what it always meant. The attachment
    // count and every format have to match the pass, as colorFormat does.
    Vector<ColorTargetState> extraColorTargets;

    int colorTargetCount() const;

    // Attachment `index` as one shape, the first included.
    ColorTargetState colorTarget(int index) const;
};

// Attachment `index` as a pipeline compiles it: as described, unless the
// pipeline has no fragment stage, when nothing reaches any attachment and the
// format alone is kept to match the pass.
ColorTargetState writtenColorTarget(const RenderPipelineDescriptor& descriptor,
                                    int index,
                                    bool hasFragment);

// The blend an attachment ends up with: the written-out equation when there is
// one, the named mode's otherwise.
BlendState resolvedBlend(const ColorTargetState& target);

// The bias a pipeline is built with: the descriptor's, or none at all when it
// has no depth test for the bias to move.
DepthBias appliedDepthBias(const RenderPipelineDescriptor& descriptor);

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

    // appliedDepthBias of the descriptor. Encoder state on Metal like the two
    // above, so it is read and set on every bind there.
    DepthBias depthBias() const;

    // Opaque native handles for cross-translation-unit use by the render pass.
    // nativeDepthState() is the combined depth-stencil state: non-null wherever
    // the pipeline was given a depth or a stencil plane - a stencil-only
    // pipeline has one, the depth test being Always with no write - and null
    // only when it has neither. Metal's render pass is its one reader; on the
    // other backends the state is inside the object nativeState() returns and
    // the handle exists to keep the three answering the same question.
    void* nativeState() const;
    void* nativeDepthState() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
