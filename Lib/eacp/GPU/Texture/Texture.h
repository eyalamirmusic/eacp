#pragma once

#include "../Common.h"

#include <cstddef>

namespace eacp::GPU
{
class Device;

enum class TextureFormat
{
    RGBA8Unorm,
    BGRA8Unorm,

    // Single 8-bit channel, sampled as (r, 0, 0, 1). The natural format for
    // palette indices, masks and other one-byte-per-pixel data.
    R8Unorm,

    // Two 8-bit channels, sampled as (r, g, 0, 1). Carries the interleaved
    // Cb/Cr plane of an NV12 video frame, whose luma plane is an R8Unorm of
    // twice the width and height.
    RG8Unorm,

    // Floating point, for a texture a shader writes as well as reads. Eight bits
    // per channel are enough to show a colour and not nearly enough to keep one:
    // a pass that feeds back into itself quantises every frame, so a value it
    // accumulates over hundreds of them - a trail, a simulation state, a running
    // average - drifts to a flat colour it can no longer leave.
    //
    // RGBA16Float is the one to reach for. Both backends can filter it on every
    // device eacp runs on, it holds well over the range a colour needs, and it
    // costs half of what the full float does. RGBA32Float is there for the
    // simulation that really needs the mantissa; filtering one is *not*
    // guaranteed, so sample it Nearest unless the device is known to allow more.
    RGBA16Float,
    RGBA32Float,

    // One full-precision channel, sampled as (r, 0, 0, 1). What a texture
    // holding a *measurement* rather than a colour wants: a depth buffer copied
    // out for a later pass to read, a distance field, a height map, a single
    // accumulated scalar. Eight and sixteen bits are both too coarse for the
    // first of those - a window-space depth spends almost its whole range in
    // the last thousandth, where a half float has about one value to offer.
    //
    // Renderable on both backends and readable back through read(); filtering
    // it is *not* guaranteed, so sample it Nearest for the same reason
    // RGBA32Float says so.
    R32Float,

    // Block-compressed: a 4x4 block of texels stored as one 8- or 16-byte
    // record that the sampler decodes on its way out. The picture costs a
    // quarter or an eighth of what it does uncompressed - in memory, over the
    // bus and in every cache in between - and it stays compressed on the
    // device, which is the whole difference between this and decompressing at
    // load.
    //
    // **eacp neither compresses nor decompresses.** These are for content that
    // arrived compressed - a .dds file, an atlas some tool produced - and the
    // blocks reach the device exactly as they came off disk. Compressing here
    // would mean choosing an encoder, and every encoder makes a different
    // picture out of the same pixels.
    //
    // BC1 is 8 bytes a block: three colour channels and, in the half of the
    // encoding where the two endpoints are ordered the other way round, a
    // one-bit alpha - DXT1 with or without punch-through is this one format on
    // both APIs. BC2 (DXT3) and BC3 (DXT5) are 16 bytes and differ only in how
    // they spend the extra eight: four explicit bits of alpha per texel, or two
    // alpha endpoints and a three-bit index. BC7 is 16 and is the modern one -
    // eight modes, and near-lossless where the older three band visibly.
    //
    // No sRGB variants, because eacp has no sRGB formats at all.
    //
    // **What a compressed texture cannot be**, refused rather than
    // half-supported and in the same words on both backends: a renderTarget or
    // a computeWrite target, there being no per-texel address for a pass or a
    // kernel to write to. read() and update(region, ...) are no-ops on one - a
    // read-back would have to hand back blocks in a layout nothing here
    // consumes, and a region would have to be block-aligned, which nothing here
    // needs. And `mipmapped` gets it exactly one level: the CPU filter averages
    // texels, and a block is not four numbers to take a mean of.
    // TextureDescriptor::mipLevels is how a compressed texture gets a chain,
    // and is the only way it can.
    //
    // Not every device has them. Every Mac does, and every feature-level-11
    // Direct3D device is required to; an Apple-family iOS GPU mostly does not.
    // Ask Device::supportsBlockCompression - a texture asking for a format the
    // device refuses is invalid, exactly as a refused sampleCount is.
    BC1RGBA,
    BC2RGBA,
    BC3RGBA,
    BC7RGBA
};

// Whether one record of this format covers a 4x4 block of texels rather than a
// single one. The formats that do are sized differently from the rest all the
// way down, which is what levelBytesPerRow and levelBytes below exist to hide
// from everything that only wants the size of a level.
constexpr bool isCompressedFormat(TextureFormat format)
{
    return format == TextureFormat::BC1RGBA || format == TextureFormat::BC2RGBA
           || format == TextureFormat::BC3RGBA || format == TextureFormat::BC7RGBA;
}

// Bytes one 4x4 block occupies, and 0 for a format that has no blocks. Both
// APIs round a level up to whole blocks, so a 2x2 level of a compressed texture
// is still one block of this size and a 1x1 level is too.
constexpr int bytesPerBlock(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::BC1RGBA:
            return 8;

        case TextureFormat::BC2RGBA:
        case TextureFormat::BC3RGBA:
        case TextureFormat::BC7RGBA:
            return 16;

        case TextureFormat::RGBA8Unorm:
        case TextureFormat::BGRA8Unorm:
        case TextureFormat::R8Unorm:
        case TextureFormat::RG8Unorm:
        case TextureFormat::RGBA16Float:
        case TextureFormat::RGBA32Float:
        case TextureFormat::R32Float:
            break;
    }

    return 0;
}

// Bytes one texel occupies, and 0 for a block-compressed format, which has no
// per-texel size at all - levelBytesPerRow is the number a compressed upload
// wants.
//
// Exhaustive on purpose. This answered 4 for anything it did not recognise
// until the block formats arrived, which is a wrong number rather than a
// missing case: a format added without a size here is now a -Wswitch warning
// instead of a texture uploaded at someone else's stride.
constexpr int bytesPerPixel(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::R8Unorm:
            return 1;

        case TextureFormat::RG8Unorm:
            return 2;

        case TextureFormat::RGBA8Unorm:
        case TextureFormat::BGRA8Unorm:
        case TextureFormat::R32Float:
            return 4;

        case TextureFormat::RGBA16Float:
            return 8;

        case TextureFormat::RGBA32Float:
            return 16;

        case TextureFormat::BC1RGBA:
        case TextureFormat::BC2RGBA:
        case TextureFormat::BC3RGBA:
        case TextureFormat::BC7RGBA:
            break;
    }

    return 0;
}

// A level's row pitch when it is tightly packed: bytes per row of texels, or
// bytes per row of *blocks* for a compressed format, where one row covers four
// rows of texels.
constexpr std::size_t levelBytesPerRow(TextureFormat format, int width)
{
    if (isCompressedFormat(format))
        return (std::size_t) ((width + 3) / 4) * (std::size_t) bytesPerBlock(format);

    return (std::size_t) width * (std::size_t) bytesPerPixel(format);
}

// How many rows of that pitch a level holds: its height in texels, or in blocks
// of four.
constexpr int levelRows(TextureFormat format, int height)
{
    return isCompressedFormat(format) ? (height + 3) / 4 : height;
}

// One level of a texture this size, tightly packed - and the unit every layout
// in this header is written in: a whole 2D texture, one face of a cube, one
// level of a chain the caller supplied.
constexpr std::size_t levelBytes(TextureFormat format, int width, int height)
{
    return levelBytesPerRow(format, width) * (std::size_t) levelRows(format, height);
}

constexpr bool isFloatFormat(TextureFormat format)
{
    return format == TextureFormat::RGBA16Float
           || format == TextureFormat::RGBA32Float
           || format == TextureFormat::R32Float;
}

// Whether a kernel may write this format. The restriction is D3D12's: a typed
// UAV store is only guaranteed for a small set of formats, and everything
// outside it depends on the device. BGRA8Unorm - the drawable's own format - is
// not in the guaranteed set, which is exactly the trap this exists to close.
//
// A texture asked for computeWrite in another format fails to create rather
// than binding as a kernel output that quietly does nothing.
constexpr bool supportsComputeWrite(TextureFormat format)
{
    return format == TextureFormat::RGBA8Unorm
           || format == TextureFormat::RGBA16Float
           || format == TextureFormat::RGBA32Float;
}

enum class TextureFilter
{
    Linear,
    Nearest
};

enum class TextureAddressMode
{
    Clamp,
    Repeat
};

// How a shader wants one of its textures sampled.
//
// This belongs to the *shader*, not to the Texture, and that is a deliberate
// break from the obvious design. D3D12 offers two ways to give a draw its
// samplers: a descriptor table, which can vary per draw and would let the
// Texture carry its own state, or a static sampler baked into the root
// signature, which cannot. eacp used the descriptor table until a Windows-on-Arm
// driver turned out to ignore the table's offset outright and resolve every
// sampler to descriptor 0 of the heap - so every texture in the process sampled
// through whichever sampler happened to be first, and no per-texture state had
// any effect. Static samplers are unaffected, being nowhere near a heap.
//
// Since the configuration space is tiny, the root signature simply declares one
// static sampler per configuration - s0..s(samplingConfigurations - 1) - and
// the emitter points every texture at the one its declared sampling picks. Two
// textures sampled the same way share a sampler, which is exactly what a
// sampler is; the alternative, one per (slot, configuration) pair, was what
// this used to be and it made the sampler registers the thing that capped how
// many textures a shader could bind. Metal has no such bug, but declares a
// sampler per texture because MSL passes them as function arguments rather
// than binding them to registers.
//
// The cost is that the sampling is fixed when the shader is compiled rather
// than when a texture is bound: one shader samples one slot exactly one way.
// A renderer that must do both - Sprites::SpriteRenderer draws smoothly scaled
// camera frames and crisp pixel art through the same code - compiles one
// program per configuration and picks between them.
struct TextureSampling
{
    TextureFilter filter = TextureFilter::Nearest;
    TextureAddressMode addressMode = TextureAddressMode::Clamp;
};

// The number of distinct sampling configurations, and the index of one. On
// D3D12 this is the entire static sampler count - a texture's sampler lands on
// register s<index>, whatever slot the texture is in - and the Metal backend
// caches this many MTLSamplerStates on the Device.
constexpr int samplingConfigurations = 4;

constexpr int samplingIndex(const TextureSampling& sampling)
{
    return (sampling.filter == TextureFilter::Linear ? 2 : 0)
           + (sampling.addressMode == TextureAddressMode::Repeat ? 1 : 0);
}

// How the texture is created. Sampler state is deliberately absent: it comes
// from the shader that samples the texture, as a TextureSampling declared on
// its texture member.
struct TextureDescriptor
{
    int width = 0;
    int height = 0;
    TextureFormat format = TextureFormat::RGBA8Unorm;

    // Whether a Frame can render into this texture as well as sample it -
    // Frame::beginPass(texture). Off by default because it is not free: the
    // resource has to be created able to be an attachment, which on D3D12 also
    // costs a render-target descriptor, and a texture that is only ever
    // uploaded to and sampled should not pay for either.
    //
    // A render target's pixels come from the GPU, so update() is not the way to
    // fill one and a null `pixels` is the only thing to create one with.
    bool renderTarget = false;

    // Whether a compute kernel can write into this texture - the other way its
    // pixels come from the GPU, and the one that lets a kernel produce
    // something a later pass samples. Off by default for the same reason
    // renderTarget is: the resource has to be created able to be one, which on
    // D3D12 also costs a UAV descriptor.
    //
    // Only the formats supportsComputeWrite() allows may ask for this, and only
    // on a device whose driver reports a typed UAV store for the format; a
    // texture that asks for it anywhere else is invalid rather than silently
    // unwritable. Unlike a render target this leaves update() alone, so a
    // kernel that accumulates can still be seeded from the CPU.
    bool computeWrite = false;

    // Whether to build the full chain of half-size levels down to 1x1, so the
    // GPU can sample a smaller one when the texture is drawn smaller than it is.
    //
    // What this buys is not speed but the picture. A texture minified without
    // mips samples a scattering of individual texels, and which texels those are
    // changes as the camera moves - so a tiled floor or a detailed model
    // shimmers and crawls at distance, and no amount of filtering at level 0
    // fixes it, because the information being aliased was thrown away before the
    // filter saw it.
    //
    // Off by default: a texture drawn at or above its own size - a UI atlas, a
    // video frame, a full-screen effect - never samples a level below 0, and
    // would pay a third more memory and upload for levels nothing reads.
    //
    // The levels are built on the CPU from the pixels passed in, so a texture
    // created with none of them (a render target, a kernel output) gets no
    // chain. update() rebuilds it; update(region, ...) does not - a partial
    // upload has no way to know what the rest of the texture holds, so it
    // refreshes level 0 and leaves the levels below it as they were.
    //
    // A block-compressed format gets exactly one level whatever this says. The
    // filter averages texels and a 4x4 block is not four numbers to take a mean
    // of, so building a chain would mean decoding, filtering and re-encoding
    // with an encoder eacp does not have. mipLevels below is how such a texture
    // gets a chain - the one its compressor already built.
    bool mipmapped = false;

    // How many levels the pixels passed in already hold, for a caller that
    // built its own chain. 0 - the default - is exactly what it was before this
    // existed: one level, or the whole chain eacp builds when `mipmapped` says
    // so.
    //
    // Above 0, `pixels` is N levels tightly packed with level 0 first, level i
    // being levelBytes(format, mipExtent(width, i), mipExtent(height, i)) - the
    // layout MipChain already produces, so a chain eacp built and a chain a
    // .dds file carries are the same block of bytes. They are uploaded as they
    // arrive, with no filter of eacp's own anywhere near them.
    //
    // **Two callers want this, for different reasons.** A compressed texture
    // has no other way to have a chain at all, blocks being unaverageable, and
    // every .dds file carries the one its compressor produced. And an
    // uncompressed one may want a filter eacp does not have: Doom 3's own mip
    // builder can preserve a zero border, so a projected light's low levels stay
    // dark at the edge where an unweighted average spills light past it.
    //
    // **A descriptor field rather than an update() overload**, because both
    // APIs fix a texture's level count when the resource is created - a chain
    // handed over afterwards would have nowhere to go. eacp's own builder stays
    // the default and becomes one way of getting a chain rather than the only
    // one.
    //
    // Refused rather than reconciled, each yielding an invalid texture: with
    // `mipmapped`, which says the opposite thing about who builds the chain;
    // above mipLevelCount(width, height), which is more levels than the size
    // has; with null pixels, there being no chain to take and nothing to fill
    // the levels with; on a renderTarget or a computeWrite texture, whose pixels
    // come from the GPU so there was never a chain to hand over; and on a cube,
    // whose faces would each carry one and where nothing here can pin which
    // level a direction sampled. And bytesPerRow must be 0 on such a texture's
    // update(), the levels being tightly packed by definition.
    int mipLevels = 0;

    // Whether a pass rendering into this texture gets a depth buffer, which is
    // what a pipeline built with RenderPipelineDescriptor::depth tests against
    // and what a drawable pass already gets from GPUView::setDepth. Off by
    // default: a full-screen pass over a whole texture has no use for one, and
    // the buffer costs the colour texture's size again.
    //
    // Without it a 3D scene cannot be rendered into a texture at all. The depth
    // test has nothing to test against, so what comes out is painter's order -
    // which is the reason this exists, and why it is not the same question as
    // multisampling.
    //
    // The buffer is created beside the colour texture and lives exactly as
    // long, so a render target stays one object with no second lifetime to keep
    // in step. It is cleared to the far plane at the start of every pass and
    // never stored, like the drawable's. Ignored without renderTarget, which is
    // what renders.
    bool depth = false;

    // Whether that depth buffer may also be *sampled*, by a pass that is not
    // the one rendering into it - RenderPass::setFragmentDepthTexture, and a
    // shader slot declared with ShaderBuilder::depthTexture.
    //
    // Implies depth, and is separate from it because it is not free: on D3D12
    // the resource has to be created typeless so that one view can read it as a
    // depth buffer and another as a single float channel, and it costs a
    // shader-visible descriptor and a pair of barriers around every pass that
    // attaches it. A target nothing samples the depth of should not pay for
    // either.
    //
    // **What it is for is a pass reading the depth an earlier pass wrote**, and
    // the earlier pass has to have ended: a texture cannot be sampled by the
    // pass rendering into it, and a depth attachment is no different. So this
    // pairs with DepthAction::Keep, which is what makes the values survive the
    // end of that pass - a soft particle fading where it meets the wall behind
    // it, a fog whose thickness is how far away the geometry is, a decal
    // projected onto whatever the depth buffer says is there.
    bool sampleableDepth = false;

    // Whether that buffer also carries a stencil plane, for a pass whose
    // pipelines set RenderPipelineDescriptor::stencil.
    //
    // Implies depth rather than standing beside it: both APIs put the two
    // planes in one attachment of one combined format, so a stencil-only target
    // would allocate the depth plane anyway and then have to explain why it is
    // there. Asking for stencil therefore gets a depth buffer as well, which a
    // pipeline is free to leave untested. Ignored without renderTarget, as
    // depth is.
    bool stencil = false;

    // How many samples a pass rendering into this target takes per pixel. 1 -
    // the default - is no multisampling, and is what every texture target was
    // before this existed.
    //
    // Above 1 the target grows a second colour texture beside itself: a
    // multisampled one that the pass actually renders into, resolved into this
    // texture at the end of every pass. So the texture a shader samples, and the
    // one read() reads, is always the resolved single-sampled picture, and
    // nothing binding a render target has to know whether it is multisampled.
    // **A pipeline drawing into it must carry the same
    // RenderPipelineDescriptor::sampleCount**, which both backends enforce.
    //
    int sampleCount = 1;

    // Whether this is a cube texture: six square faces sampled with a direction
    // rather than with a coordinate, which is what a reflection or a sky needs.
    // Declare it in the shader with ShaderBuilder::cubeTexture (or a
    // Uniform<TextureCube> member) and sample it with a Float3.
    //
    // **The six faces arrive as one block of pixels, in +X, -X, +Y, -Y, +Z, -Z
    // order, each face width * height of the format's bytesPerPixel with row 0
    // at the top.** That is one convention rather than three: Metal's cube slice
    // order, D3D12's array order under a TEXTURECUBE view and OpenGL's
    // GL_TEXTURE_CUBE_MAP_POSITIVE_X + i are the same six faces in the same
    // sequence, and all three orient a face the same way - which is what lets a
    // cube assembled for any one of them be uploaded here untouched.
    //
    // It is worth pinning rather than assuming, because there is nothing to warn
    // you when it is wrong: a face in the wrong slot, or flipped within its own
    // slot, still samples and still looks like a picture.
    // Tests/GPU/CubeTextureTests.cpp holds both halves - which face each axis
    // reads, and which way u and v run inside one.
    //
    // width and height must be equal; six faces of a rectangle is a shape
    // neither API has. A cube asking for renderTarget or computeWrite is refused
    // rather than half-supported, there being no way here to say which face a
    // pass or a kernel would write. A cube may be compressed - the six faces are
    // each levelBytes of blocks, in the same order - but not with a supplied
    // mipLevels, for the reason that field gives.
    //
    // mipmapped builds a chain per face out of that face's own pixels, which is
    // what both APIs' own generators do: no level is ever averaged across a
    // seam.
    bool cube = false;
};

// A texture sampled by the fragment stage (MTLTexture on Metal, a D3D12
// resource with its SRV descriptor on Windows). Create via Device::makeTexture
// with tightly packed pixels, row 0 at the top, or null pixels for an
// uninitialised texture. Bind with RenderPass::setFragmentTexture.
//
// **What "tightly packed" means is levelBytes of the format**, which is the
// format's bytesPerPixel per texel for the ordinary formats and one 8- or
// 16-byte record per 4x4 block for the compressed ones. The block is one level
// of one face unless the descriptor asked for more: six faces in a row for a
// cube, mipLevels levels in a row for a chain the caller built.
//
// Two dimensionalities, decided by TextureDescriptor::cube: a 2D image sampled
// with a Float2, or six square faces sampled with a Float3 direction. They are
// one class because everything below the sample is the same - one resource, one
// descriptor, one upload path, one bind - and the difference lives where it is
// actually visible, which is the shader's declaration.
class Texture
{
public:
    Texture(Device& device, const TextureDescriptor& descriptor, const void* pixels);

    // Wraps an existing platform pixel buffer (a CVPixelBuffer on macOS) as a
    // sampleable texture without copying its pixels — the zero-copy path for
    // camera and video frames. The buffer must outlive the texture. Yields an
    // invalid texture on backends without zero-copy support (Windows for now),
    // where update() is the per-frame upload path instead.
    Texture(Device& device, void* nativePixelBuffer);

    int width() const;
    int height() const;
    bool isValid() const;

    // Whether this texture is the six-faced kind, which is what a shader
    // declaring a TextureCube has to be handed. Binding a 2D texture where the
    // shader declared a cube is a mismatch neither backend reports: Metal draws
    // nothing through the sampler and D3D12 reads an SRV of the wrong
    // dimension, so this is the only thing that can tell the two apart at the
    // bind.
    bool isCube() const;

    // Whether this texture was created able to be rendered into, which is what
    // Frame::beginPass(texture) needs and what makes an ordinary one a no-op
    // there rather than undefined behaviour.
    bool isRenderTarget() const;

    // Whether a kernel can write into this texture - what
    // ComputePass::setOutputTexture needs, and false on a texture whose format
    // or device refused the request.
    bool isComputeWritable() const;

    // How many mip levels this texture actually has: 1 unless it asked for a
    // chain and got one, and TextureDescriptor::mipLevels where the caller
    // supplied the chain itself. A format the chain builder does not know how to
    // average - every compressed one - yields 1 rather than a texture whose
    // lower levels are uninitialised.
    int mipLevels() const;

    // Whether a pass into this texture carries a depth buffer, which is what a
    // depth-tested pipeline needs and false on a target that did not ask for
    // one. A pass runs either way; without this it runs without the test.
    bool hasDepth() const;

    // Whether that buffer carries a stencil plane too - what a pipeline setting
    // RenderPipelineDescriptor::stencil needs to match. True implies hasDepth,
    // the two planes being one attachment.
    bool hasStencil() const;

    // How many samples a pass into this target takes, which is what a pipeline
    // drawing here has to set RenderPipelineDescriptor::sampleCount to. 1 on
    // everything that is not a multisampled render target, including a target
    // whose requested count the device refused - such a texture is invalid, so
    // there is no case where this quietly disagrees with what was asked for.
    int sampleCount() const;

    // Whether that buffer can be sampled as well as attached, which is what
    // RenderPass::setFragmentDepthTexture needs and false on a target created
    // without TextureDescriptor::sampleableDepth. True implies hasDepth. A bind
    // through a target that answers false is a no-op rather than a read of
    // something undefined.
    bool hasSampleableDepth() const;

    // Re-uploads pixels into a texture created by Device::makeTexture, reusing
    // the GPU resource instead of allocating a new one — the per-frame path for
    // video and camera streams. Source rows are tightly packed unless
    // bytesPerRow gives a larger stride (0 means width * the format's
    // bytesPerPixel), matching the padded rows capture buffers often carry. A
    // no-op on a wrapped or invalid texture, or when pixels is null.
    //
    // On a cube this takes all six faces, in the order and layout the
    // descriptor's `cube` describes - the same block of pixels the texture was
    // created from. There is no way to replace one face, for the reason there
    // is no region form below.
    //
    // On a compressed texture, or one created with TextureDescriptor::mipLevels,
    // it takes the same block the constructor took - the blocks of every level,
    // tightly packed - and **bytesPerRow must be 0**. Both layouts are tightly
    // packed by definition, so a stride there is a number that can only be
    // wrong; a nonzero one is a no-op rather than an upload at a pitch nothing
    // means.
    void update(const void* pixels, std::size_t bytesPerRow = 0);

    // Re-uploads one sub-rectangle, leaving the rest of the texture untouched.
    //
    // The reason this exists: a glyph atlas grows one glyph at a time, and
    // whole-texture update() makes each new glyph cost an upload of the entire
    // atlas — megabytes to move a few hundred bytes. Here the transfer is the
    // size of the region.
    //
    // region is in texels with the origin at the top-left. pixels points at the
    // region's own top-left, and its rows are tightly packed to the *region's*
    // width unless bytesPerRow gives a larger stride — so a glyph can be
    // uploaded straight out of a larger rasterization buffer by passing that
    // buffer's stride.
    //
    // A region that is empty, or that is not wholly inside the texture, is a
    // no-op. Deliberately not clamped: a clamped region would keep consuming
    // source rows at the original width and silently upload skewed pixels,
    // which is far harder to spot than nothing appearing.
    //
    // A no-op on a cube, which has six rectangles this could mean and no
    // argument to say which. Quietly writing +X would be exactly the kind of
    // silent wrong answer the out-of-bounds rule above exists to avoid.
    //
    // A no-op on a compressed texture too. A region there would have to start
    // and end on a 4x4 block boundary, so the rect a caller wrote and the rect
    // the GPU updated would differ by up to three texels a side - and nothing
    // that wants this wants it for compressed content, a glyph atlas being
    // uploaded a glyph at a time as it is rasterized.
    void update(const Graphics::Rect& region,
                const void* pixels,
                std::size_t bytesPerRow = 0);

    // Copies the texture's pixels back to the CPU — update()'s mirror, and the
    // only way what the GPU produced becomes bytes a program can look at: a
    // screenshot, a kernel's output image, a test asserting on what a pass
    // actually drew.
    //
    // dst receives rows tightly packed at the format's bytesPerPixel with row 0
    // at the top, unless bytesPerRow gives a larger stride — the same layout
    // update() reads, so a texture read out and uploaded back is the texture it
    // was. It must have room for the texture's (or the region's) height of them.
    //
    // **Valid once the work that drew the texture has been committed**, which is
    // the rule Buffer::read carries and the same trap. A pass recorded on a
    // Frame has not been: the frame's commands reach the GPU when the frame
    // ends, so a read inside the frame that drew it reads what was there
    // before. Frame::flush() is what makes it true, and is there for this.
    //
    // The copy goes through a staging buffer and blocks until the GPU has
    // finished it, so this is a stall by construction — a round trip to the
    // device per call. That is what a screenshot costs; it is not something to
    // put in a frame loop.
    //
    // A no-op on an invalid texture, a null dst, or a region that is not wholly
    // inside the texture — not clamped, for the reason update() gives. Also on a
    // cube, which has six faces and no way here to name one, exactly as the
    // region update has not.
    //
    // And a no-op on a compressed texture. Handing back the blocks would need a
    // layout of its own and a decoder at the other end, and handing back texels
    // would need a decoder here - while the things this exists for, a screenshot
    // and a test's assertion, read a render target, which a compressed texture
    // cannot be.
    void read(void* dst, std::size_t bytesPerRow = 0) const;
    void read(const Graphics::Rect& region,
              void* dst,
              std::size_t bytesPerRow = 0) const;

    // Opaque native handles for cross-translation-unit use by the render pass.
    // There is no sampler handle: the render pass gets that from the sampling
    // the shader declared, not from the texture.
    void* nativeTexture() const;

    // The read view the fragment stage binds on D3D12 (the same handle as
    // nativeTexture). Null on Metal, where the texture is bound directly.
    void* nativeReadView() const;

    // The depth buffer a pass into this texture attaches, or null on a target
    // that asked for none. Multisampled when the target is. An MTLTexture on
    // Metal; on D3D12 the depth resource and its descriptor live inside the same
    // texture data nativeTexture hands back, so this is null there and Frame
    // reaches them through that.
    void* nativeDepthTexture() const;

    // The multisampled colour texture a pass into this target actually renders
    // into, resolved into nativeTexture at the end of every pass. Null on a
    // single-sampled target, which renders into its own texture directly, and
    // null on D3D12, where it lives in the texture data with everything else.
    void* nativeMultisampleTexture() const;

    // The single-sampled depth buffer the multisampled one resolves into, which
    // is what setFragmentDepthTexture binds. Null unless the target is both
    // multisampled and sampleableDepth - on a single-sampled target the
    // attachment is already sampleable and nativeDepthTexture is the answer -
    // and null on D3D12 for the reason above.
    void* nativeResolvedDepthTexture() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
