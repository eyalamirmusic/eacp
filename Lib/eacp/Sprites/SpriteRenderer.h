#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/GPU/GPU.h>

#include <optional>

namespace eacp::Sprites
{
// Every coordinate and colour in this module is one of these, so they are
// hoisted here rather than qualified on each of a few hundred mentions. Scoped
// to the module deliberately: pulling them into all of eacp would collide with
// any user type of the same name reached through `using namespace eacp`.
using Graphics::Color;
using Graphics::Point;
using Graphics::Rect;

// A unit-quad corner (each component 0 or 1) the shader maps onto the
// destination parallelogram. The only per-vertex data there is: everything that
// differs between quads is per-instance, below.
struct SpriteVertex
{
    float corner[2];
};

// One quad. Everything that varies from quad to quad lives here so that a run
// of them sharing a texture is a single instanced draw, rather than a draw, a
// uniform upload and a texture bind apiece.
struct SpriteInstance
{
    // The destination parallelogram in logical units: where the texture's
    // top-left lands, and where its +u and +v axes go from there.
    float origin[2];
    float edgeX[2];
    float edgeY[2];

    // The sampled sub-rect, in normalised texture coordinates.
    float uv0[2];
    float uv1[2];

    float tint[4];
};

// The sprite shader: a unit quad mapped onto a parallelogram (an origin plus two
// edge vectors, in logical units), sampling a sub-rect of the bound texture,
// multiplied by a tint. The parallelogram form lets one draw path cover both
// axis-aligned rects (perpendicular edges) and arbitrarily oriented quads such
// as thick lines.
//
// The quad itself is per-instance, so one draw covers as many of them as share a
// texture; only the screen size and the texture are uniforms.
struct SpriteShader final : GPU::ShaderProgram
{
    explicit SpriteShader(GPU::TextureSampling sampling)
    {
        image.sampling = sampling;
        compile();
    }

    void define() override;

    GPU::Uniform<GPU::Float2> screenSize;
    GPU::Uniform<GPU::Texture2D> image;

    EACP_SHADER(screenSize, image)
};

// How to turn NV12 samples into RGB, supplied per draw because the matrix and
// coding range belong to the video track rather than to the renderer. The Video
// module derives one of these from a frame (see Video::YuvTransform, which this
// mirrors field for field); Sprites only applies the numbers.
//
//     R = y + redV * v
//     G = y - greenU * u - greenV * v
//     B = y + blueU * u
struct YuvTransform
{
    float lumaOffset = 0.0f;
    float lumaScale = 1.0f;
    float chromaOffset = 0.0f;
    float chromaScale = 1.0f;

    float redV = 0.0f;
    float greenU = 0.0f;
    float greenV = 0.0f;
    float blueU = 0.0f;
};

// The same quad as SpriteShader, but sampling a video frame's two NV12 planes
// and converting to RGB here rather than on the CPU.
//
// This lives with the sprite shader because it is the same parallelogram, the
// same tint and the same pipeline machinery — only the fragment colour is
// derived differently. Giving video its own renderer would duplicate all of
// that to change one expression.
//
// Unlike the sprite shader this one is not instanced, and deliberately: a video
// frame is one quad per draw, so the quad stays in uniforms, where a batch of
// one would only add machinery.
struct Nv12Shader final : GPU::ShaderProgram
{
    explicit Nv12Shader(GPU::TextureSampling sampling)
    {
        luma.sampling = sampling;
        chroma.sampling = sampling;
        compile();
    }

    void define() override;

    GPU::Uniform<GPU::Float2> screenSize;
    GPU::Uniform<GPU::Float2> origin;
    GPU::Uniform<GPU::Float2> edgeX;
    GPU::Uniform<GPU::Float2> edgeY;
    GPU::Uniform<GPU::Float4> tint;

    // (lumaOffset, lumaScale, chromaOffset, chromaScale) and
    // (redV, greenU, greenV, blueU) — see Sprites::YuvTransform. Uniforms
    // rather than shader constants because the matrix belongs to the track, and
    // a player does not get to choose it.
    GPU::Uniform<GPU::Float4> yuvRange;
    GPU::Uniform<GPU::Float4> yuvMatrix;

    // Full-resolution single-channel luma, and the half-resolution plane
    // carrying Cb in r and Cr in g. Both are sampled with the same 0-1
    // coordinates: the hardware handles the resolution difference, and the
    // linear filter on the chroma plane is the upsampling.
    GPU::Uniform<GPU::Texture2D> luma;
    GPU::Uniform<GPU::Texture2D> chroma;

    EACP_SHADER(
        screenSize, origin, edgeX, edgeY, tint, yuvRange, yuvMatrix, luma, chroma)
};

// 2D sprite renderer: textured quads and untextured primitives (drawn with a
// 1x1 white texture) share one always-blended pipeline per sampling
// configuration. Drawing happens in a logical space the shader maps to clip
// space. Every coordinate is a float, so callers keep full sub-pixel precision -
// smooth motion, high-DPI, fractional zoom - and snap only where they choose to.
//
// Draws are batched. A quad is queued onto a run rather than drawn on the spot,
// and the run becomes one instanced draw as soon as something makes the next
// quad unable to join it: a different texture or sampling, a scissor change, a
// video quad, or the end of the pass. Order is never rearranged, because with
// blending on, order *is* the picture - a run is always a contiguous span of the
// calls the caller made. A screen of text out of one font texture, or a floor
// out of one tile, therefore costs a single draw.
//
// None of which the caller has to know: begin(pass) joins the pass as a
// RenderPass::Participant, so whatever is still queued is drawn when the pass
// ends. Batching is an implementation detail rather than a protocol.
//
//     sprites.begin(pass);
//     sprites.fillRect(...);
//     sprites.drawTexture(...);
//     // pass ends; the queue goes with it
//
// A caller that changes pass state by hand - RenderPass::setScissorRect, a
// pipeline of its own - does still have to say when, since quads queued before
// the change would otherwise be drawn under it rather than under the state they
// were issued in. That is what flush() is for, and why the scissor wrappers
// below are worth preferring: they do it themselves.
class SpriteRenderer : public GPU::RenderPass::Participant
{
public:
    // logicalSize is the logical space draws are expressed in; sampleCount must
    // match the render pass's MSAA sample count. colorFormat must match the
    // attachment the pass writes: the default suits a view's drawable, and a
    // renderer drawing into a texture (Frame::beginPass(texture)) passes
    // GPU::pixelFormatFor(that texture's format) instead.
    SpriteRenderer(Point logicalSizeToUse,
                   int sampleCountToUse,
                   GPU::PixelFormat colorFormatToUse = GPU::PixelFormat::BGRA8Unorm);

    ~SpriteRenderer() override;

    // Call once per frame with the pass, before any draw call. Joins the pass,
    // which draws anything still queued when it ends.
    void begin(GPU::RenderPass& passToUse);

    // Draws whatever is still queued and leaves the pass early. Optional: the
    // pass does this itself when it ends. Reach for it only to stop drawing
    // through this renderer while the pass carries on - and note that beginning
    // a new pass does not need it, since the old pass drained this renderer on
    // its way out.
    void end();

    // Draws whatever is queued without leaving the pass. What to call before
    // changing pass state by hand, so the quads issued under the old state are
    // not drawn under the new one.
    void flush();

    // The logical space draws are expressed in. Cheap, because it is a uniform
    // and not anything compiled: a view that resizes sets it again rather than
    // rebuilding the renderer and recompiling its pipelines.
    void setLogicalSize(Point size);
    Point getLogicalSize() const { return logicalSize; }

    // Draws the queued quads and then clips the pass, so that what was issued
    // before the call escapes the clip. Same units as
    // RenderPass::setScissorRect, which is render-target *pixels*: a caller
    // working in logical points multiplies by GPUView::backingScale() first.
    void setScissorRect(const Rect& rectInPixels);
    void clearScissorRect();

    // The whole texture stretched to dst, optionally mirrored. The default tint
    // is opaque white, i.e. the texture's own colours.
    //
    // sampling defaults to Nearest, which is what pixel art and 1:1 blits want.
    // Pass Linear for anything scaled by an arbitrary factor - a camera frame
    // fitted to a view, a zoomed photo - or it aliases. Each configuration
    // costs one extra compiled pipeline, built the first time it is asked for.
    void drawTexture(const GPU::Texture& texture,
                     const Rect& dst,
                     bool flipX = false,
                     bool flipY = false,
                     const Color& tint = Color::white(),
                     GPU::TextureSampling sampling = {});

    // The src sub-rect (in texels) of the texture stretched to dst.
    void drawTexture(const GPU::Texture& texture,
                     const Rect& src,
                     const Rect& dst,
                     const Color& tint = Color::white(),
                     GPU::TextureSampling sampling = {});

    // The whole texture mapped onto an arbitrary parallelogram: `origin` is
    // where the texture's top-left lands, and the two edge vectors are where
    // its +u and +v axes go, in logical units.
    //
    // Every other draw here is a special case of this. It is public because
    // orientation is not always expressible as a flip: a video track carrying a
    // 90-degree display rotation needs the u axis to run down the screen, which
    // no combination of flipX/flipY can produce.
    void drawTextureQuad(const GPU::Texture& texture,
                         Point origin,
                         Point edgeX,
                         Point edgeY,
                         const Color& tint = Color::white(),
                         GPU::TextureSampling sampling = {});

    // As drawTextureQuad, but the image comes from a video frame's two NV12
    // planes and is converted to RGB in the shader. `luma` is the full-size
    // single-channel plane and `chroma` the half-size Cb/Cr one.
    void drawNv12Quad(const GPU::Texture& luma,
                      const GPU::Texture& chroma,
                      const YuvTransform& transform,
                      Point origin,
                      Point edgeX,
                      Point edgeY,
                      const Color& tint = Color::white(),
                      GPU::TextureSampling sampling = {});

    void fillRect(const Rect& rect, const Color& color);

    // An outline drawn inside the rect's edges, `thickness` logical units wide.
    void drawRect(const Rect& rect, const Color& color, float thickness = 1.0f);

    // A straight line of the given thickness between two points, any orientation.
    void drawLine(Point a, Point b, const Color& color, float thickness = 1.0f);

private:
    // The core primitive: a textured parallelogram (origin + the two edge
    // vectors) sampling the [uv0, uv1] sub-rect, multiplied by tint. Queues it
    // onto the open run, flushing first when this quad cannot join that run.
    void addQuad(const GPU::Texture& texture,
                 Point origin,
                 Point edgeX,
                 Point edgeY,
                 float u0,
                 float v0,
                 float u1,
                 float v1,
                 const Color& tint,
                 GPU::TextureSampling sampling);

    // The pass is closing and this is the last chance to draw. Flushes without
    // touching the pass's participant list, which it is in the middle of.
    void flushInto(GPU::RenderPass& endingPass) override;

    // Leaves the pass, if still in one.
    void detach();

    SpriteShader& programFor(GPU::TextureSampling sampling);
    Nv12Shader& nv12ProgramFor(GPU::TextureSampling sampling);

    Point logicalSize;
    int sampleCount = 1;
    GPU::PixelFormat colorFormat = GPU::PixelFormat::BGRA8Unorm;

    Array<std::optional<SpriteShader>, GPU::samplingConfigurations> programs;

    // Built on first use like the sprite programs, so an app that never draws
    // video never compiles a YUV shader.
    Array<std::optional<Nv12Shader>, GPU::samplingConfigurations> nv12Programs;

    // The open run: the quads queued so far, and what all of them share. The
    // texture is referred to and not retained, so it has to outlive the flush -
    // which it does, a run never outliving the pass it was queued in.
    Vector<SpriteInstance> instances;
    const GPU::Texture* batchTexture = nullptr;
    GPU::TextureSampling batchSampling {};

    GPU::Texture white;
    GPU::RenderPass* pass = nullptr;
};
} // namespace eacp::Sprites
