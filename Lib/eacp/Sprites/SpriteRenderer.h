#pragma once

#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Primitives/Primitives.h>

namespace eacp::Sprites
{
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
    SpriteShader() { compile(); }

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

// 2D sprite renderer: textured quads and untextured primitives (drawn with a
// 1x1 white texture) share one always-blended pipeline. Drawing happens in a
// fixed logical space, sized at construction; the shader maps it to clip space.
// Every coordinate is a float, so callers keep full sub-pixel precision - smooth
// motion, high-DPI, fractional zoom - and snap only where they choose to.
class SpriteRenderer
{
public:
    // logicalSize is the logical space draws are expressed in; sampleCount must
    // match the render pass's MSAA sample count.
    SpriteRenderer(Graphics::Point logicalSize, int sampleCount);

    // Call once per frame with the pass, before any draw call.
    void begin(GPU::RenderPass& passToUse);

    // The whole texture stretched to dst, optionally mirrored. The default tint
    // is opaque white, i.e. the texture's own colours.
    void drawTexture(const GPU::Texture& texture,
                     const Graphics::Rect& dst,
                     bool flipX = false,
                     bool flipY = false,
                     const Graphics::Color& tint = Graphics::Color::white());

    // The src sub-rect (in texels) of the texture stretched to dst.
    void drawTexture(const GPU::Texture& texture,
                     const Graphics::Rect& src,
                     const Graphics::Rect& dst,
                     const Graphics::Color& tint = Graphics::Color::white());

    void fillRect(const Graphics::Rect& rect, const Graphics::Color& color);

    // An outline drawn inside the rect's edges, `thickness` logical units wide.
    void drawRect(const Graphics::Rect& rect,
                  const Graphics::Color& color,
                  float thickness = 1.0f);

    // A straight line of the given thickness between two points, any orientation.
    void drawLine(Graphics::Point a,
                  Graphics::Point b,
                  const Graphics::Color& color,
                  float thickness = 1.0f);

private:
    // The core primitive: a textured parallelogram (origin + the two edge
    // vectors) sampling the [uv0, uv1] sub-rect, multiplied by tint.
    void drawQuad(const GPU::Texture& texture,
                  Graphics::Point origin,
                  Graphics::Point edgeX,
                  Graphics::Point edgeY,
                  float u0,
                  float v0,
                  float u1,
                  float v1,
                  const Graphics::Color& tint);

    SpriteShader shader;
    GPU::ShaderLibrary library;
    GPU::RenderPipeline pipeline;
    GPU::Texture white;
    GPU::RenderPass* pass = nullptr;
};
} // namespace eacp::Sprites
