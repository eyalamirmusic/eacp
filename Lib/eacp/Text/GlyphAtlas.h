#pragma once

#include "GlyphRasterizer.h"
#include "ShelfPacker.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>

namespace eacp::Text
{
// Where a cached glyph lives and how to place it. Returned by value: a
// reference into the cache would dangle the first time the atlas reset.
struct GlyphSlot
{
    // Texel rect within the atlas texture of this slot's format.
    Graphics::Rect src;

    // Relative to the pen, in points: x from the pen, y from the baseline
    // downwards to the bitmap's top edge, so pen + offset is the dest top-left.
    Graphics::Point offset;

    // Pen advance in points.
    float advance = 0.f;

    GlyphFormat format = GlyphFormat::Mask;

    // False means no face had a glyph for the codepoint.
    bool valid = false;

    // A space: valid and advances the pen, but has nothing to draw.
    bool empty = false;
};

// Caches rasterized glyphs in two GPU textures packed on demand -- masks in an
// R8Unorm atlas, colour glyphs in an RGBA8 one -- so a caller draws two batches
// and tints only the first. Many faces share one atlas, at one device scale.
class GlyphAtlas
{
public:
    // How a face is built: a GlyphRasterizer in production, a stub in a test.
    using FaceFactory =
        std::function<OwningPointer<GlyphSource>(const FontRequest&)>;

    // `defaultFace` is face 0, and supplies the scale everything is rasterized
    // at.
    explicit GlyphAtlas(FaceFactory factoryToUse,
                        const FontRequest& defaultFace = {},
                        int initialSize = 512,
                        int maxSize = 4096);

    ~GlyphAtlas();

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    // Built on first ask, sizes matching within faceSizeTolerance. The index
    // stays valid for the life of the atlas, including across a scale change.
    int findOrAddFace(const std::string& family, float pointSize);

    int findOrAddFace(const Font& font)
    {
        return findOrAddFace(font.family, font.pointSize);
    }

    int faceCount() const { return faces.size(); }

    // Rasterizes on first request. An out-of-range face draws nothing.
    GlyphSlot glyph(char32_t codepoint, FontStyle style, int face = 0);

    // In points, unlike the pixel-space FontMetrics the rasterizer reports.
    FontMetrics metrics(FontStyle style = FontStyle::Regular, int face = 0) const;

    float scale() const { return deviceScale; }

    // Device pixels per point. Changing it clears and rebuilds every face,
    // since one texture cannot hold bitmaps for two scales. Indices survive.
    void setScale(float newScale);

    // Uploads whatever changed. Call once per frame after every glyph the frame
    // needs has been requested and before the first draw that samples them.
    void commit();

    GPU::Texture& maskTexture();
    GPU::Texture& colorTexture();

    // Bumped when the atlas clears, invalidating every slot handed out before.
    // A caller that caches slots across frames re-requests when it changes.
    std::uint32_t generation() const { return atlasGeneration; }

    int size() const { return atlasSize; }
    float occupancy() const { return maskPacker.occupancy(); }

private:
    struct Page;

    GlyphSlot insert(char32_t codepoint, FontStyle style, int face);
    bool place(ShelfPacker& packer, const GlyphBitmap& bitmap, PackedRect& out);
    void growOrReset();

    // Both the way out of a full atlas and what a scale change needs.
    void dropEverything();

    OwningPointer<GlyphSource> makeSource(const std::string& family,
                                          float pointSize) const;

    static std::uint64_t keyFor(char32_t codepoint, FontStyle style, int face);

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

    struct Page
    {
        std::vector<std::uint8_t> pixels;
        std::optional<GPU::Texture> texture;

        // Bounding box of everything written since the last commit, so a new
        // glyph costs its own size to upload rather than the whole atlas.
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

GlyphAtlas::FaceFactory rasterizerFaceFactory();
} // namespace eacp::Text
