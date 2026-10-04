#pragma once

#include <eacp/GPU/Codegen/Codegen.h>

namespace eacp::Sprites
{
// The sprite shader: a unit quad mapped onto a parallelogram (an origin plus two
// edge vectors, in logical units), sampling a sub-rect of the bound texture,
// multiplied by a tint. The parallelogram form lets one draw path cover both
// axis-aligned rects (perpendicular edges) and arbitrarily oriented quads such
// as thick lines.
//
// The quad itself is per-instance, so one draw covers as many of them as share a
// texture; only the screen size and the texture are uniforms. Its vertex and
// instance layouts are SpriteVertex and SpriteInstance, in SpriteRenderer.h.
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
} // namespace eacp::Sprites
