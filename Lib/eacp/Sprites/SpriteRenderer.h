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
// destination parallelogram.
struct SpriteVertex
{
    float corner[2];
};

// The sprite shader: a unit quad mapped onto a parallelogram (an origin plus two
// edge vectors, in logical units), sampling a sub-rect of the bound texture,
// multiplied by a tint. The parallelogram form lets one draw path cover both
// axis-aligned rects (perpendicular edges) and arbitrarily oriented quads such
// as thick lines.
struct SpriteShader final : GPU::ShaderProgram
{
    explicit SpriteShader(GPU::TextureSampling sampling)
    {
        image.sampling = sampling;
        compile();
    }

    void define() override;

    GPU::Uniform<GPU::Float2> screenSize;
    GPU::Uniform<GPU::Float2> origin;
    GPU::Uniform<GPU::Float2> edgeX;
    GPU::Uniform<GPU::Float2> edgeY;
    GPU::Uniform<GPU::Float2> uv0;
    GPU::Uniform<GPU::Float2> uv1;
    GPU::Uniform<GPU::Float4> tint;
    GPU::Uniform<GPU::Texture2D> image;

    EACP_SHADER(screenSize, origin, edgeX, edgeY, uv0, uv1, tint, image)
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

// The shader, library and pipeline for one sampling configuration.
//
// Sampling is baked in when the shader is compiled (see GPU::TextureSampling),
// so a renderer cannot re-point one program at a different filter between
// draws - and this one has to serve both, drawing smoothly scaled camera frames
// and crisp pixel art through the same calls. Hence a program per
// configuration, built on first use: most renderers only ever touch one.
struct SpriteProgram
{
    SpriteProgram(GPU::TextureSampling sampling, Point logicalSize, int sampleCount);

    SpriteShader shader;
    GPU::ShaderLibrary library;
    GPU::RenderPipeline pipeline;
};

struct Nv12Program
{
    Nv12Program(GPU::TextureSampling sampling, Point logicalSize, int sampleCount);

    Nv12Shader shader;
    GPU::ShaderLibrary library;
    GPU::RenderPipeline pipeline;
};

// 2D sprite renderer: textured quads and untextured primitives (drawn with a
// 1x1 white texture) share one always-blended pipeline per sampling
// configuration. Drawing happens in a fixed logical space, sized at
// construction; the shader maps it to clip space. Every coordinate is a float,
// so callers keep full sub-pixel precision - smooth motion, high-DPI,
// fractional zoom - and snap only where they choose to.
class SpriteRenderer
{
public:
    // logicalSize is the logical space draws are expressed in; sampleCount must
    // match the render pass's MSAA sample count.
    SpriteRenderer(Point logicalSize, int sampleCount);

    // Call once per frame with the pass, before any draw call.
    void begin(GPU::RenderPass& passToUse);

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
    // vectors) sampling the [uv0, uv1] sub-rect, multiplied by tint.
    void drawQuad(const GPU::Texture& texture,
                  Point origin,
                  Point edgeX,
                  Point edgeY,
                  float u0,
                  float v0,
                  float u1,
                  float v1,
                  const Color& tint,
                  GPU::TextureSampling sampling);

    SpriteProgram& programFor(GPU::TextureSampling sampling);
    Nv12Program& nv12ProgramFor(GPU::TextureSampling sampling);

    Point logicalSize;
    int sampleCount = 1;

    Array<std::optional<SpriteProgram>, GPU::samplingConfigurations> programs;

    // Built on first use like the sprite programs, so an app that never draws
    // video never compiles a YUV shader.
    Array<std::optional<Nv12Program>, GPU::samplingConfigurations> nv12Programs;

    // Which pipeline is currently bound on the pass, or -1 when none is:
    // begin() clears it, and a draw that needs a different program rebinds.
    // Sprite programs occupy 0..samplingConfigurations-1 and the NV12 ones the
    // range above, so one field orders both. Switching mid-frame costs a
    // pipeline change, so callers that mix should batch where it is easy.
    int boundProgram = -1;

    GPU::Texture white;
    GPU::RenderPass* pass = nullptr;
};
} // namespace eacp::Sprites
