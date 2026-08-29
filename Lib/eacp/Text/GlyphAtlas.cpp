#include "GlyphAtlas.h"
#include "Utf8.h"

#include <algorithm>
#include <cstring>

namespace eacp::Text
{
namespace
{
// Padding between packed glyphs. One texel of transparent gutter is enough to
// stop linear sampling pulling a neighbour's coverage into a glyph's edge.
constexpr int glyphPadding = 1;

// Shaped strings kept before the cache starts over.
constexpr std::size_t maxShapedStrings = 16384;
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

GlyphAtlas::GlyphAtlas(FaceFactory factoryToUse,
                       const FontRequest& defaultFace,
                       int initialSize,
                       int maxSize)
    : factory(std::move(factoryToUse))
    , deviceScale(defaultFace.scale > 0.f ? defaultFace.scale : 1.f)
    , atlasSize(std::max(initialSize, 64))
    , maxAtlasSize(std::max(maxSize, std::max(initialSize, 64)))
    , maskPacker(atlasSize, atlasSize, glyphPadding)
    , colorPacker(atlasSize, atlasSize, glyphPadding)
{
    resizePage(maskPage, atlasSize, 1);
    resizePage(colorPage, atlasSize, 4);

    findOrAddFace(defaultFace.family, defaultFace.pointSize);
}

GlyphAtlas::~GlyphAtlas() = default;

OwningPointer<GlyphSource> GlyphAtlas::makeSource(const std::string& family,
                                                  float pointSize) const
{
    auto request = FontRequest {};
    request.family = family;
    request.pointSize = pointSize;
    request.scale = deviceScale;

    return factory(request);
}

int GlyphAtlas::findOrAddFace(const std::string& family, float pointSize)
{
    for (auto index = 0; index < faces.size(); ++index)
        if (sameFace({faces[index].family, faces[index].pointSize},
                     {family, pointSize}))
            return index;

    auto source = makeSource(family, pointSize);

    // A factory that produced nothing leaves the caller drawing in the default
    // face, which is a wrong family rather than an empty string.
    if (source == nullptr)
        return 0;

    faces.emplace_back(Face {family, pointSize, std::move(source)});

    return faces.size() - 1;
}

void GlyphAtlas::setScale(float newScale)
{
    const auto scale = newScale > 0.f ? newScale : 1.f;

    if (scale == deviceScale)
        return;

    deviceScale = scale;

    // Rebuilt in place, so every index a caller is holding still names the same
    // family and size.
    for (auto& face: faces)
        face.source = makeSource(face.family, face.pointSize);

    dropEverything();
}

std::uint64_t GlyphAtlas::keyFor(
    GlyphKey key, const FontVariant& variant, int face, int phase, bool lightText)
{
    // A glyph id is sixteen bits in every font format, which leaves the low
    // word room for the font it is in, the weight class, the slant, the phase
    // and the lightness; the face gets a word of its own.
    const auto weight =
        static_cast<std::uint64_t>(weightClass(variant.weight) / 100);
    const auto font = static_cast<std::uint64_t>(std::clamp(key.font, 0, 255));

    return static_cast<std::uint64_t>(key.glyph & 0xFFFF) | (font << 16)
           | (weight << 24) | (static_cast<std::uint64_t>(variant.italic) << 28)
           | (static_cast<std::uint64_t>(phase & 3) << 29)
           | (static_cast<std::uint64_t>(lightText) << 31)
           | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(face)) << 32);
}

std::string GlyphAtlas::shapeKeyFor(std::string_view text,
                                    const FontVariant& variant,
                                    int face)
{
    auto key = std::string {};
    key.reserve(text.size() + 4);
    key += static_cast<char>(face);
    key += static_cast<char>(face >> 8);
    key += static_cast<char>(weightClass(variant.weight) / 100
                             + (variant.italic ? 16 : 0));
    key += text;

    return key;
}

FontMetrics GlyphAtlas::metrics(const FontVariant& variant, int face) const
{
    if (face < 0 || face >= faces.size())
        return {};

    const auto& source = faces[face].source;

    auto pixels = source->metrics(variant);
    const auto scale = source->scale() > 0.f ? source->scale() : 1.f;

    // The rasterizer works in device pixels; callers lay out in points.
    return {pixels.ascent / scale,
            pixels.descent / scale,
            pixels.leading / scale,
            pixels.advance / scale};
}

GlyphSlot GlyphAtlas::glyph(GlyphKey glyphKey,
                            const FontVariant& variant,
                            int face,
                            int phase,
                            bool lightText)
{
    if (face < 0 || face >= faces.size())
        return {};

    const auto clampedPhase = std::clamp(phase, 0, subpixelPhases - 1);
    const auto key = keyFor(glyphKey, variant, face, clampedPhase, lightText);
    const auto found = slots.find(key);

    if (found != slots.end())
        return found->second;

    const auto slot = insert(glyphKey, variant, face, clampedPhase, lightText);
    slots.emplace(key, slot);

    return slot;
}

GlyphSlot GlyphAtlas::glyph(char32_t codepoint, FontStyle style, int face)
{
    char encoded[4] = {};
    const auto length = encodeUtf8(codepoint, encoded);
    const auto run = shape({encoded, length}, variantOf(style), face);

    if (run.glyphs.empty())
        return {};

    return run.glyphs[0].slot;
}

ShapedText
    GlyphAtlas::shape(std::string_view text, const FontVariant& variant, int face)
{
    if (face < 0 || face >= faces.size() || text.empty())
        return {};

    const auto key = shapeKeyFor(text, variant, face);
    const auto found = shaped.find(key);

    if (found != shaped.end())
        return found->second;

    const auto& source = faces[face].source;
    const auto scale = source->scale() > 0.f ? source->scale() : 1.f;
    const auto run = source->shape(text, variant);

    auto result = ShapedText {};
    result.advance = run.advance / scale;

    for (const auto& placed: run.glyphs)
        result.glyphs.add({glyph(placed.key, variant, face),
                           placed.key,
                           {placed.x / scale, -placed.y / scale},
                           placed.cluster});

    // A document's vocabulary is bounded; a cache that is not would hold every
    // string ever measured, so it starts over rather than grow without end.
    if (shaped.size() >= maxShapedStrings)
        shaped.clear();

    shaped.emplace(std::move(key), result);

    return result;
}

GlyphSlot GlyphAtlas::insert(
    GlyphKey key, const FontVariant& variant, int face, int phase, bool lightText)
{
    const auto& source = faces[face].source;
    const auto request = RasterRequest {
        static_cast<float>(phase) / static_cast<float>(subpixelPhases), lightText};
    const auto bitmap = source->rasterize(key, variant, request);

    if (!bitmap.valid)
        return {};

    // A colour glyph is placed on whole pixels and carries its own colours, so
    // its phases and lightnesses share the one bitmap rather than costing the
    // atlas eight copies of every emoji.
    if (bitmap.format == GlyphFormat::Color && (phase != 0 || lightText))
        return glyph(key, variant, face, 0, false);

    const auto scale = source->scale() > 0.f ? source->scale() : 1.f;

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

bool GlyphAtlas::place(ShelfPacker& packer,
                       const GlyphBitmap& bitmap,
                       PackedRect& out)
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
    dropEverything();
}

void GlyphAtlas::dropEverything()
{
    maskPacker.clear();
    colorPacker.clear();
    slots.clear();
    shaped.clear();

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
    auto resized = std::vector<std::uint8_t>(
        static_cast<std::size_t>(newSize) * newSize * stride, std::uint8_t {0});

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

void GlyphAtlas::blit(Page& page,
                      const GlyphBitmap& bitmap,
                      const PackedRect& at,
                      int stride)
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

GlyphAtlas::FaceFactory rasterizerFaceFactory()
{
    return [](const FontRequest& request)
    { return OwningPointer<GlyphSource> {makeOwned<GlyphRasterizer>(request)}; };
}
} // namespace eacp::Text
