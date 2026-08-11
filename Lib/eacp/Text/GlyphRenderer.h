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

// Everything varying per glyph, so a screen of text is a single draw call.
struct GlyphInstance
{
    // Destination rect in logical points: x, y, width, height.
    float rect[4];

    // Source rect in atlas texels: x, y, width, height.
    float source[4];

    float color[4];
};

// Draws glyphs from a GlyphAtlas, batched and instanced. Not a SpriteRenderer:
// a mask samples as (coverage, 0, 0, 1) and needs its coverage moved into
// alpha, and a screenful of glyphs has to be one draw rather than thousands.
class GlyphRenderer
{
public:
    GlyphRenderer();

    // Defined in the .cpp, where Program is complete: an implicit one would
    // delete an incomplete type here.
    ~GlyphRenderer();

    GlyphRenderer(const GlyphRenderer&) = delete;
    GlyphRenderer& operator=(const GlyphRenderer&) = delete;

    // Logical size of the surface being drawn into, for the pixel-to-clip
    // mapping. Cheap to call every frame.
    void setViewportSize(Graphics::Point size);

    void begin();

    // Mask and colour glyphs queue separately, sampling different textures: a
    // mask is tinted, a colour glyph is drawn as-is.
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
