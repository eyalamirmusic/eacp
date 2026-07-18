#pragma once

#include "GlyphRasterizer.h"
#include "ShelfPacker.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace eacp::Text
{
// Where a cached glyph lives and how to place it.
//
// Returned **by value**. CowTerm's atlas returned a reference into its cache,
// which a later reset invalidated while callers were still holding it — a
// dangling read waiting for the first time the atlas filled up. A slot is four
// floats and two flags; copying is cheaper than the hazard.
struct GlyphSlot
{
    // Texel rect within the atlas texture of this slot's format.
    Graphics::Rect src;

    // Where to put the bitmap relative to the pen, in **points**: x from the
    // pen, y from the baseline downwards to the bitmap's top edge. Adding these
    // to a pen position gives the destination rect's top-left directly.
    Graphics::Point offset;

    // Pen advance in points.
    float advance = 0.f;

    GlyphFormat format = GlyphFormat::Mask;

    // The font can draw this codepoint. False means no face had a glyph.
    bool valid = false;

    // Valid, advances the pen, but has nothing to draw — a space. Callers skip
    // the draw and still step the pen.
    bool empty = false;
};

// Caches rasterized glyphs in a GPU texture, packed on demand.
//
// Two textures, not one: masks go into an R8Unorm atlas (a quarter the memory
// of RGBA8, and all a coverage mask needs) and colour glyphs into an RGBA8 one.
// A caller draws in two batches, one per texture, tinting the first and not the
// second.
//
// Growth over eviction: when the atlas fills it doubles, up to maxSize, and
// existing glyphs keep their coordinates — shelf placements only ever extend
// right and down, so nothing needs re-rasterizing. Only at maxSize does it
// clear, and generation() ticks so callers can notice.
class GlyphAtlas
{
public:
    // Owns its source. Pass a GlyphRasterizer in production, a stub in tests.
    explicit GlyphAtlas(OwningPointer<GlyphSource> source,
                        int initialSize = 512,
                        int maxSize = 4096);

    ~GlyphAtlas();

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    // Rasterizes on first request, then returns the cached slot.
    GlyphSlot glyph(char32_t codepoint, FontStyle style);

    // Face metrics in **points**, unlike the pixel-space FontMetrics the
    // rasterizer reports.
    FontMetrics metrics(FontStyle style = FontStyle::Regular) const;

    // Uploads whatever changed since the last call, then returns the textures.
    //
    // Call once per frame *after* every glyph the frame needs has been
    // requested and before the first draw that samples them. Uploading in the
    // middle of a pass would mutate a texture the earlier draws already bound.
    void commit();

    GPU::Texture& maskTexture();
    GPU::Texture& colorTexture();

    // Bumped when the atlas is cleared, which invalidates every slot handed out
    // before it. A caller that caches slots across frames re-requests when this
    // changes; one that requests every glyph each frame can ignore it.
    std::uint32_t generation() const { return atlasGeneration; }

    int size() const { return atlasSize; }
    float occupancy() const { return maskPacker.occupancy(); }

private:
    struct Page;

    GlyphSlot insert(char32_t codepoint, FontStyle style);
    bool place(ShelfPacker& packer, const GlyphBitmap& bitmap, PackedRect& out);
    void growOrReset();

    static std::uint32_t keyFor(char32_t codepoint, FontStyle style);

    OwningPointer<GlyphSource> glyphSource;

    int atlasSize = 0;
    int maxAtlasSize = 0;
    std::uint32_t atlasGeneration = 0;

    ShelfPacker maskPacker;
    ShelfPacker colorPacker;

    std::unordered_map<std::uint32_t, GlyphSlot> slots;

    struct Page
    {
        std::vector<std::uint8_t> pixels;
        std::optional<GPU::Texture> texture;

        // Bounding box of everything written since the last commit. Uploading
        // just this is what makes a new glyph cost its own size rather than the
        // whole atlas.
        int dirtyLeft = 0;
        int dirtyTop = 0;
        int dirtyRight = 0;
        int dirtyBottom = 0;
        bool dirty = false;
        bool needsFullUpload = true;

        void markDirty(int x, int y, int width, int height);
        void clearDirty();
    };

    Page maskPage;
    Page colorPage;

    void resizePage(Page& page, int newSize, int bytesPerPixel);
    void uploadPage(Page& page, GPU::TextureFormat format, int bytesPerPixel);
    void blit(Page& page, const GlyphBitmap& bitmap, const PackedRect& at, int bytesPerPixel);
};
} // namespace eacp::Text
