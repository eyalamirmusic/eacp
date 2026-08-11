#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/GPU/GPU.h>

#include <optional>

namespace eacp::Sprites
{
using Graphics::Color;
using Graphics::Point;
using Graphics::Rect;

// A unit-quad corner (each component 0 or 1) the shader maps onto the
// destination parallelogram.
struct SpriteVertex
{
    float corner[2];
};

struct SpriteInstance
{
    // Destination parallelogram in logical units: where the texture's top-left
    // lands, and where its +u and +v axes go from there.
    float origin[2];
    float edgeX[2];
    float edgeY[2];

    // Sampled sub-rect, in normalised texture coordinates.
    float uv0[2];
    float uv1[2];

    float tint[4];
};

// A unit quad mapped onto a parallelogram, sampling a sub-rect of the bound
// texture, multiplied by a tint. The quad is per-instance, so one draw covers
// as many quads as share a texture.
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

// NV12 -> RGB, supplied per draw since matrix and coding range belong to the
// video track. Mirrors Video::YuvTransform field for field.
//     R = y + redV * v; G = y - greenU * u - greenV * v; B = y + blueU * u
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

// The same quad as SpriteShader, sampling a video frame's two NV12 planes and
// converting to RGB in the shader. Not instanced: a video frame is one quad
// per draw, so the quad stays in uniforms.
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
    // (redV, greenU, greenV, blueU) — see Sprites::YuvTransform.
    GPU::Uniform<GPU::Float4> yuvRange;
    GPU::Uniform<GPU::Float4> yuvMatrix;

    // Full-resolution single-channel luma, and the half-resolution plane
    // carrying Cb in r and Cr in g. Both sampled with the same 0-1 coordinates;
    // the linear filter on the chroma plane is the upsampling.
    GPU::Uniform<GPU::Texture2D> luma;
    GPU::Uniform<GPU::Texture2D> chroma;

    EACP_SHADER(
        screenSize, origin, edgeX, edgeY, tint, yuvRange, yuvMatrix, luma, chroma)
};

// 2D sprite renderer. Quads queue into a run and flush as one instanced draw
// when the next cannot join it, in issue order. begin(pass) joins the pass,
// which drains the queue when it ends; hand-made state changes need flush().
class SpriteRenderer : public GPU::RenderPass::Participant
{
public:
    // sampleCount must match the pass's MSAA sample count, colorFormat the
    // attachment it writes: the default suits a view's drawable, a renderer
    // drawing into a texture passes GPU::pixelFormatFor(its format).
    SpriteRenderer(Point logicalSizeToUse,
                   int sampleCountToUse,
                   GPU::PixelFormat colorFormatToUse = GPU::PixelFormat::BGRA8Unorm);

    ~SpriteRenderer() override;

    // Call once per frame, before any draw call.
    void begin(GPU::RenderPass& passToUse);

    // Optional: the pass drains this renderer itself when it ends.
    void end();

    // Draws whatever is queued without leaving the pass.
    void flush();

    void setLogicalSize(Point size);
    Point getLogicalSize() const { return logicalSize; }

    // Same units as RenderPass::setScissorRect, which is render-target *pixels*:
    // a caller working in logical points multiplies by GPUView::backingScale().
    void setScissorRect(const Rect& rectInPixels);
    void clearScissorRect();

    // sampling defaults to Nearest, which suits pixel art and 1:1 blits. Pass
    // Linear for anything scaled by an arbitrary factor or it aliases. Each
    // configuration costs one pipeline, compiled on first use.
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

    // `origin` is where the texture's top-left lands, and the two edge vectors
    // are where its +u and +v axes go, in logical units. Public because a
    // 90-degree display rotation is not expressible as a flip.
    void drawTextureQuad(const GPU::Texture& texture,
                         Point origin,
                         Point edgeX,
                         Point edgeY,
                         const Color& tint = Color::white(),
                         GPU::TextureSampling sampling = {});

    // As drawTextureQuad, but from a video frame's two NV12 planes. `luma` is
    // the full-size single-channel plane and `chroma` the half-size Cb/Cr one.
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

    void drawLine(Point a, Point b, const Color& color, float thickness = 1.0f);

private:
    // Queues a textured parallelogram sampling the [uv0, uv1] sub-rect onto the
    // open run, flushing first when this quad cannot join it.
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

    // Flushes without touching the pass's participant list, which the ending
    // pass is in the middle of walking.
    void flushInto(GPU::RenderPass& endingPass) override;

    void detach();

    SpriteShader& programFor(GPU::TextureSampling sampling);
    Nv12Shader& nv12ProgramFor(GPU::TextureSampling sampling);

    Point logicalSize;
    int sampleCount = 1;
    GPU::PixelFormat colorFormat = GPU::PixelFormat::BGRA8Unorm;

    Array<std::optional<SpriteShader>, GPU::samplingConfigurations> programs;
    Array<std::optional<Nv12Shader>, GPU::samplingConfigurations> nv12Programs;

    // The open run. batchTexture is referred to and not retained, so it has to
    // outlive the flush - which it does, a run never outliving its pass.
    Vector<SpriteInstance> instances;
    const GPU::Texture* batchTexture = nullptr;
    GPU::TextureSampling batchSampling {};

    GPU::Texture white;
    GPU::RenderPass* pass = nullptr;
};
} // namespace eacp::Sprites
