#pragma once

#include "GlyphRasterizer.h"
#include "ShelfPacker.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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

// One glyph of a shaped string with its slot: where the glyph's own pen sits
// relative to the string's, in points — x along the line, y downwards — so
// the destination's top-left is the string's pen plus this plus the slot's
// offset. `cluster` is the byte offset of the character it came from. The
// slot is the glyph at phase 0; `key` is what to ask for another phase by.
struct ShapedSlot
{
    GlyphSlot slot;
    GlyphKey key;
    Graphics::Point pen;
    int cluster = 0;
};

// A string shaped and cached: every glyph's slot, and the advance in points.
struct ShapedText
{
    Vector<ShapedSlot> glyphs;
    float advance = 0.f;
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
//
// **One atlas holds many faces.** A face is a (family, pointSize) pair, and the
// key is (face, variant, font, glyph id, phase, lightness) — so a heading, a caption and a
// monospace log share one texture and are drawn in one batch. The alternative, an atlas per
// size, is what this class used to be: it costs a texture and a batch break per
// size, which is bearable for an interface that looks like one thing and wrong
// for a document, which mixes sizes by definition.
//
// The size is the caller's, in points; the *scale* is the atlas's, because
// every glyph in it has to be rasterized for the same display.
class GlyphAtlas
{
public:
    // How a face is built. Production hands over a GlyphRasterizer; a test
    // hands over a stub, which is what lets the packing, growth, upload and
    // face-table logic be exercised with no font and no GPU.
    using FaceFactory =
        std::function<OwningPointer<GlyphSource>(const FontRequest&)>;

    // `defaultFace` is face 0 and supplies the scale everything is rasterized
    // at. A caller that never asks for another face gets exactly the atlas this
    // class used to be.
    explicit GlyphAtlas(FaceFactory factoryToUse,
                        const FontRequest& defaultFace = {},
                        int initialSize = 512,
                        int maxSize = 4096);

    ~GlyphAtlas();

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    // The index of the face this family and size name, built on first ask.
    // Sizes match within faceSizeTolerance -- see the note there for why an
    // exact match is the wrong test.
    //
    // Held by the caller across frames: an index stays valid for the life of
    // the atlas, including across a scale change, which rebuilds what is behind
    // each one rather than renumbering them.
    int findOrAddFace(const std::string& family, float pointSize);

    int findOrAddFace(const Font& font)
    {
        return findOrAddFace(font.family, font.pointSize);
    }

    int faceCount() const { return faces.size(); }

    // Shapes the string through the face's source and returns each glyph with
    // its slot, rasterizing what the atlas has not seen. Shaped strings are
    // cached too — a document measures the same words over and over — and the
    // cache goes with the slots whenever the atlas is cleared.
    ShapedText
        shape(std::string_view text, const FontVariant& variant, int face = 0);

    // Rasterizes on first request, then returns the cached slot. An out-of-range
    // face draws nothing rather than reading past the table.
    //
    // `phase` is which of the subpixelPhases positions within a pixel the
    // glyph is drawn at, 0 being the pixel's own left edge, and `lightText`
    // whether it is drawn in light text, which the platform thickens more
    // (RasterRequest); each combination is a slot of its own, rasterized on
    // first ask. A colour glyph has one — emoji sit on whole pixels and carry
    // their own colours — so its other phases hand back the same slot.
    GlyphSlot glyph(GlyphKey key,
                    const FontVariant& variant,
                    int face = 0,
                    int phase = 0,
                    bool lightText = false);

    // The one glyph a codepoint shapes to on its own: the way to walk a string
    // by codepoint when nothing between the codepoints matters, and what a
    // shaped run is made of when the face has no kerning or ligatures.
    GlyphSlot glyph(char32_t codepoint, FontStyle style, int face = 0);

    // Face metrics in **points**, unlike the pixel-space FontMetrics the
    // rasterizer reports.
    FontMetrics metrics(const FontVariant& variant, int face = 0) const;
    FontMetrics metrics(FontStyle style = FontStyle::Regular, int face = 0) const
    {
        return metrics(variantOf(style), face);
    }

    float scale() const { return deviceScale; }

    // Device pixels per point for everything in the atlas. Changing it clears
    // and rebuilds every face: a bitmap rasterized for one display is the wrong
    // bitmap for another, and keeping them would mix two scales in one texture.
    // Face indices survive, so a caller holding them needs no fixing up.
    void setScale(float newScale);

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

    GlyphSlot insert(GlyphKey key,
                     const FontVariant& variant,
                     int face,
                     int phase,
                     bool lightText);
    bool place(ShelfPacker& packer, const GlyphBitmap& bitmap, PackedRect& out);
    void growOrReset();

    // Everything cached, dropped. Both the way out of a full atlas and what a
    // scale change needs, which is why it is not simply part of growOrReset.
    void dropEverything();

    OwningPointer<GlyphSource> makeSource(const std::string& family,
                                          float pointSize) const;

    static std::uint64_t keyFor(GlyphKey key,
                                const FontVariant& variant,
                                int face,
                                int phase,
                                bool lightText);
    static std::string
        shapeKeyFor(std::string_view text, const FontVariant& variant, int face);

    // One family and size, and the rasterizer that draws it at this atlas's
    // scale.
    struct Face
    {
        std::string family;
        float pointSize = 0.f;
        OwningPointer<GlyphSource> source;
    };

    FaceFactory factory;
    Vector<Face> faces;

    float deviceScale = 1.f;

    int atlasSize = 0;
    int maxAtlasSize = 0;
    std::uint32_t atlasGeneration = 0;

    ShelfPacker maskPacker;
    ShelfPacker colorPacker;

    std::unordered_map<std::uint64_t, GlyphSlot> slots;
    std::unordered_map<std::string, ShapedText> shaped;

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
    void blit(Page& page,
              const GlyphBitmap& bitmap,
              const PackedRect& at,
              int bytesPerPixel);
};

// The factory everything outside a test uses: one GlyphRasterizer per face.
GlyphAtlas::FaceFactory rasterizerFaceFactory();
} // namespace eacp::Text
