#include "GlyphAtlas.h"

#include <algorithm>
#include <cstring>

namespace eacp::Text
{
namespace
{
// Padding between packed glyphs. One texel of transparent gutter is enough to
// stop linear sampling pulling a neighbour's coverage into a glyph's edge.
constexpr int glyphPadding = 1;
} // namespace

void GlyphAtlas::Page::markDirty(int x, int y, int width, int height)
{
    if (!dirty)
    {
        dirtyLeft = x;
        dirtyTop = y;
        dirtyRight = x + width;
        dirtyBottom = y + height;
        dirty = true;
        return;
    }

    dirtyLeft = std::min(dirtyLeft, x);
    dirtyTop = std::min(dirtyTop, y);
    dirtyRight = std::max(dirtyRight, x + width);
    dirtyBottom = std::max(dirtyBottom, y + height);
}

void GlyphAtlas::Page::clearDirty()
{
    dirty = false;
    dirtyLeft = 0;
    dirtyTop = 0;
    dirtyRight = 0;
    dirtyBottom = 0;
}

GlyphAtlas::GlyphAtlas(OwningPointer<GlyphSource> source, int initialSize, int maxSize)
    : glyphSource(std::move(source))
    , atlasSize(std::max(initialSize, 64))
    , maxAtlasSize(std::max(maxSize, std::max(initialSize, 64)))
    , maskPacker(atlasSize, atlasSize, glyphPadding)
    , colorPacker(atlasSize, atlasSize, glyphPadding)
{
    resizePage(maskPage, atlasSize, 1);
    resizePage(colorPage, atlasSize, 4);
}

GlyphAtlas::~GlyphAtlas() = default;

std::uint32_t GlyphAtlas::keyFor(char32_t codepoint, FontStyle style)
{
    // Codepoints stop at 0x10FFFF, so the top bits are free for the style.
    return static_cast<std::uint32_t>(codepoint)
           | (static_cast<std::uint32_t>(style) << 24);
}

FontMetrics GlyphAtlas::metrics(FontStyle style) const
{
    auto pixels = glyphSource->metrics(style);
    const auto scale = glyphSource->scale() > 0.f ? glyphSource->scale() : 1.f;

    // The rasterizer works in device pixels; callers lay out in points.
    return {pixels.ascent / scale,
            pixels.descent / scale,
            pixels.leading / scale,
            pixels.advance / scale};
}

GlyphSlot GlyphAtlas::glyph(char32_t codepoint, FontStyle style)
{
    const auto key = keyFor(codepoint, style);
    const auto found = slots.find(key);

    if (found != slots.end())
        return found->second;

    const auto slot = insert(codepoint, style);
    slots.emplace(key, slot);

    return slot;
}

GlyphSlot GlyphAtlas::insert(char32_t codepoint, FontStyle style)
{
    const auto bitmap = glyphSource->rasterize(codepoint, style);

    if (!bitmap.valid)
        return {};

    const auto scale = glyphSource->scale() > 0.f ? glyphSource->scale() : 1.f;

    auto slot = GlyphSlot {};
    slot.valid = true;
    slot.format = bitmap.format;
    slot.advance = bitmap.advance / scale;
    slot.offset = {bitmap.bearingX / scale, -bitmap.bearingY / scale};

    // A space rasterizes to nothing but still advances the pen, so it is cached
    // as a valid slot with no source rect rather than re-rasterized every time.
    if (bitmap.isEmpty())
    {
        slot.empty = true;
        return slot;
    }

    const auto colored = bitmap.format == GlyphFormat::Color;
    auto& page = colored ? colorPage : maskPage;
    auto& packer = colored ? colorPacker : maskPacker;
    const auto stride = bytesPerPixel(bitmap.format);

    auto at = PackedRect {};

    // place() grows or clears on the way through, so a second failure means the
    // glyph is larger than a whole atlas — nothing can be done with that.
    if (!place(packer, bitmap, at))
        return {};

    blit(page, bitmap, at, stride);

    slot.src = {static_cast<float>(at.x),
                static_cast<float>(at.y),
                static_cast<float>(bitmap.width),
                static_cast<float>(bitmap.height)};

    return slot;
}

bool GlyphAtlas::place(ShelfPacker& packer, const GlyphBitmap& bitmap, PackedRect& out)
{
    if (const auto placed = packer.add(bitmap.width, bitmap.height))
    {
        out = *placed;
        return true;
    }

    growOrReset();

    if (const auto placed = packer.add(bitmap.width, bitmap.height))
    {
        out = *placed;
        return true;
    }

    return false;
}

void GlyphAtlas::growOrReset()
{
    if (atlasSize < maxAtlasSize)
    {
        const auto newSize = std::min(atlasSize * 2, maxAtlasSize);

        resizePage(maskPage, newSize, 1);
        resizePage(colorPage, newSize, 4);

        // Placements survive a grow — shelves only extend right and down — so
        // no glyph is re-rasterized and no slot handed out becomes stale.
        maskPacker.grow(newSize, newSize);
        colorPacker.grow(newSize, newSize);

        atlasSize = newSize;
        return;
    }

    // At the cap. Everything goes, and the generation tells callers so.
    maskPacker.clear();
    colorPacker.clear();
    slots.clear();

    std::fill(maskPage.pixels.begin(), maskPage.pixels.end(), std::uint8_t {0});
    std::fill(colorPage.pixels.begin(), colorPage.pixels.end(), std::uint8_t {0});

    maskPage.needsFullUpload = true;
    colorPage.needsFullUpload = true;
    maskPage.clearDirty();
    colorPage.clearDirty();

    ++atlasGeneration;
}

void GlyphAtlas::resizePage(Page& page, int newSize, int stride)
{
    const auto oldSize = atlasSize;
    auto resized =
        std::vector<std::uint8_t>(static_cast<std::size_t>(newSize) * newSize * stride,
                                  std::uint8_t {0});

    // Copy row by row: the old rows are shorter than the new ones, so the
    // contents keep their coordinates and every placement stays correct.
    if (!page.pixels.empty() && oldSize > 0 && oldSize <= newSize)
    {
        const auto oldRow = static_cast<std::size_t>(oldSize) * stride;
        const auto newRow = static_cast<std::size_t>(newSize) * stride;

        for (auto y = 0; y < oldSize; ++y)
            std::memcpy(&resized[static_cast<std::size_t>(y) * newRow],
                        &page.pixels[static_cast<std::size_t>(y) * oldRow],
                        oldRow);
    }

    page.pixels = std::move(resized);

    // The texture object is the wrong size now; drop it so the next commit
    // makes a new one.
    page.texture.reset();
    page.needsFullUpload = true;
    page.clearDirty();
}

void GlyphAtlas::blit(Page& page, const GlyphBitmap& bitmap, const PackedRect& at, int stride)
{
    const auto sourceRow = bitmap.bytesPerRow();

    for (auto y = 0; y < bitmap.height; ++y)
    {
        const auto destOffset =
            (static_cast<std::size_t>(at.y + y) * atlasSize + at.x) * stride;

        std::memcpy(&page.pixels[destOffset],
                    &bitmap.pixels[static_cast<std::size_t>(y) * sourceRow],
                    sourceRow);
    }

    page.markDirty(at.x, at.y, bitmap.width, bitmap.height);
}

void GlyphAtlas::uploadPage(Page& page, GPU::TextureFormat format, int stride)
{
    if (!page.texture)
    {
        auto descriptor = GPU::TextureDescriptor {};
        descriptor.width = atlasSize;
        descriptor.height = atlasSize;
        descriptor.format = format;

        // Linear so a glyph drawn at a fractional position or a non-integer
        // zoom resamples smoothly rather than shimmering.
        descriptor.filter = GPU::TextureFilter::Linear;

        page.texture.emplace(
            GPU::Device::shared().makeTexture(descriptor, page.pixels.data()));

        page.needsFullUpload = false;
        page.clearDirty();
        return;
    }

    if (page.needsFullUpload)
    {
        page.texture->update(page.pixels.data());
        page.needsFullUpload = false;
        page.clearDirty();
        return;
    }

    if (!page.dirty)
        return;

    // Only the changed rows, and only the changed span of each — the whole
    // reason Texture::update takes a region. A new glyph costs its own area
    // rather than the entire atlas.
    const auto x = page.dirtyLeft;
    const auto y = page.dirtyTop;
    const auto width = page.dirtyRight - page.dirtyLeft;
    const auto height = page.dirtyBottom - page.dirtyTop;

    const auto rowBytes = static_cast<std::size_t>(atlasSize) * stride;
    const auto* start = &page.pixels[static_cast<std::size_t>(y) * rowBytes
                                     + static_cast<std::size_t>(x) * stride];

    page.texture->update({static_cast<float>(x),
                          static_cast<float>(y),
                          static_cast<float>(width),
                          static_cast<float>(height)},
                         start,
                         rowBytes);

    page.clearDirty();
}

void GlyphAtlas::commit()
{
    uploadPage(maskPage, GPU::TextureFormat::R8Unorm, 1);
    uploadPage(colorPage, GPU::TextureFormat::RGBA8Unorm, 4);
}

GPU::Texture& GlyphAtlas::maskTexture()
{
    if (!maskPage.texture)
        commit();

    return *maskPage.texture;
}

GPU::Texture& GlyphAtlas::colorTexture()
{
    if (!colorPage.texture)
        commit();

    return *colorPage.texture;
}
} // namespace eacp::Text
