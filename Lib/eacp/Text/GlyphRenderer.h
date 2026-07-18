#pragma once

#include "GlyphAtlas.h"

#include <vector>

namespace eacp::Text
{
// A unit-quad corner, each component 0 or 1, mapped onto each glyph's rect.
struct GlyphQuadCorner
{
    float corner[2];
};

// One glyph to draw. Everything varying per glyph lives here so a whole screen
// of text is a single draw call.
struct GlyphInstance
{
    // Destination rect in logical points: x, y, width, height.
    float rect[4];

    // Source rect in atlas texels: x, y, width, height.
    float source[4];

    float color[4];
};

// Draws glyphs from a GlyphAtlas, batched and instanced.
//
// Lives here rather than in each app because without it a GlyphAtlas cannot
// actually be drawn: two reasons, and both would otherwise be reimplemented
// identically by every consumer.
//
//  1. **Correctness.** The mask atlas is R8Unorm, which samples as
//     (coverage, 0, 0, 1) — the coverage is in red and the alpha is a
//     meaningless 1. Sprites::SpriteRenderer multiplies the sample by its tint,
//     so a mask drawn through it comes out opaque red rather than tinted text.
//     A glyph shader has to move coverage into the *alpha* channel instead:
//     `float4(colour.rgb, colour.a * coverage)`.
//
//  2. **Cost.** SpriteRenderer issues one draw call per quad, with a fresh
//     uniform upload and texture bind each time. A screenful of code is
//     thousands of glyphs; here it is one drawInstanced.
class GlyphRenderer
{
public:
    GlyphRenderer();

    // Defined in the .cpp, where Program is complete. Without it the implicit
    // destructor would delete an incomplete type here in the header, which is
    // undefined behaviour rather than merely a warning.
    ~GlyphRenderer();

    GlyphRenderer(const GlyphRenderer&) = delete;
    GlyphRenderer& operator=(const GlyphRenderer&) = delete;

    // Logical size of the surface being drawn into, for the pixel-to-clip
    // mapping. Cheap to call every frame — unlike SpriteRenderer, whose logical
    // size is baked at construction and forces a rebuild on every resize.
    void setViewportSize(Graphics::Point size);

    void begin();

    // Queues one glyph. Mask and colour glyphs go to separate queues because
    // they sample different textures and shade differently: a mask is tinted,
    // a colour glyph carries its own colour and is drawn as-is.
    void add(const Graphics::Rect& destination,
             const Graphics::Rect& source,
             const Graphics::Color& color,
             bool colored);

    // Submits the queued glyphs: at most two draw calls, one per atlas.
    void flush(GPU::RenderPass& pass, GlyphAtlas& atlas);

    std::size_t queuedGlyphs() const { return masks.size() + colors.size(); }

private:
    struct Program;

    void drawQueue(GPU::RenderPass& pass,
                   std::vector<GlyphInstance>& queue,
                   GPU::Texture& texture,
                   bool colored);

    OwningPointer<Program> maskProgram;
    OwningPointer<Program> colorProgram;

    std::vector<GlyphInstance> masks;
    std::vector<GlyphInstance> colors;

    Graphics::Point viewport {1.f, 1.f};
    bool prepared = false;
};
} // namespace eacp::Text
