#include "GlyphRasterizer.h"
#include "Utf8.h"

#include <eacp/Core/Utils/Strings.h>

#include <emscripten/emscripten.h>

#include <cmath>
#include <string>
#include <unordered_map>

// The browser's own text engine, Canvas2D: a CSS font string for the face and
// size, measureText for metrics and advances, and fillText into a 2D canvas
// whose alpha is read back through getImageData. No font library is linked; the
// fonts and their fallback chains are whatever the browser has.
//
// Shaping is per code point, as on Android: a glyph key is the code point
// itself and each advances by measureText of it alone, so there is no kerning,
// no ligature and no complex-script shaping. Colour emoji come out as masks, in
// the text's colour rather than their own.

namespace eacp::Text
{
namespace
{
// One canvas for every rasterizer: an OffscreenCanvas where there is one, else
// a detached <canvas>.
EM_JS(void, webTextEnsureContext, (), {
    if (Module.eacpText)
        return;

    var canvas = typeof OffscreenCanvas != 'undefined'
                     ? new OffscreenCanvas(1, 1)
                     : document.createElement('canvas');

    Module.eacpText = canvas.getContext('2d', {willReadFrequently : true});
});

EM_JS(double, webTextWidth, (const char* font, const char* text), {
    var context = Module.eacpText;
    context.font = UTF8ToString(font);
    return context.measureText(UTF8ToString(text)).width;
});

// A family is present when text set in it measures differently from text set
// in the fallback it names, for either fallback.
EM_JS(bool, webTextHasFamily, (const char* family), {
    var context = Module.eacpText;
    var sample = 'mmmmmmmmmmlliWW@#0';
    var name = JSON.stringify(UTF8ToString(family));

    return [ 'monospace', 'sans-serif' ].some(function(fallback) {
        context.font = '72px ' + fallback;
        var plain = context.measureText(sample).width;
        context.font = '72px ' + name + ', ' + fallback;
        return context.measureText(sample).width != plain;
    });
});

// ascent, descent and the advance of "M", in pixels.
EM_JS(double, webTextFontMetric, (const char* font, int which), {
    var context = Module.eacpText;
    context.font = UTF8ToString(font);

    var measured = context.measureText('M');

    if (which == 0)
        return measured.fontBoundingBoxAscent;

    if (which == 1)
        return measured.fontBoundingBoxDescent;

    return measured.width;
});

// The ink's box around the origin, in pixels: 0 left, 1 top, 2 right,
// 3 bottom, y down.
EM_JS(double, webTextInkBound, (const char* font, const char* text, int side), {
    var context = Module.eacpText;
    context.font = UTF8ToString(font);

    var measured = context.measureText(UTF8ToString(text));
    var sides = [
        -measured.actualBoundingBoxLeft,
        -measured.actualBoundingBoxAscent,
        measured.actualBoundingBoxRight,
        measured.actualBoundingBoxDescent
    ];
    return sides[side];
});

// Draws `text` with its origin at (x, y) into a width x height canvas and
// copies the coverage into `out`, one byte a pixel.
EM_JS(void,
      webTextDraw,
      (const char* font,
       const char* text,
       int width,
       int height,
       double x,
       double y,
       unsigned char* out),
      {
          var context = Module.eacpText;
          var canvas = context.canvas;

          if (canvas.width < width)
              canvas.width = width;

          if (canvas.height < height)
              canvas.height = height;

          context.clearRect(0, 0, width, height);
          context.font = UTF8ToString(font);
          context.fillStyle = '#fff';
          context.textBaseline = 'alphabetic';
          context.textAlign = 'left';
          context.fillText(UTF8ToString(text), x, y);

          var pixels = context.getImageData(0, 0, width, height).data;

          for (var i = 0; i < width * height; ++i)
              HEAPU8[out + i] = pixels[i * 4 + 3];
      });

// Families a caller asks for meaning "the fixed-pitch face": the platforms'
// stock ones, which a browser may not have, and CSS's own keyword.
bool isMonospaceFamily(const std::string& family)
{
    for (const auto* name: {"monospace",
                            "Menlo",
                            "Menlo-Regular",
                            "Menlo-Bold",
                            "Monaco",
                            "Consolas",
                            "DejaVu Sans Mono",
                            "Courier",
                            "Courier New",
                            "Droid Sans Mono",
                            "Roboto Mono",
                            defaultMonospaceFamily()})
        if (Strings::equalsCaseInsensitive(family, name))
            return true;

    return false;
}

bool isGenericFamily(const std::string& family)
{
    for (const auto* name: {"sans-serif", "serif", "system-ui", "default", ""})
        if (Strings::equalsCaseInsensitive(family, name))
            return true;

    return false;
}

std::string utf8Of(char32_t codepoint)
{
    char bytes[4];
    return {bytes, (std::size_t) encodeUtf8(codepoint, bytes)};
}

struct VariantKey
{
    int weight = 400;
    bool italic = false;

    bool operator==(const VariantKey&) const = default;
};

struct VariantKeyHash
{
    std::size_t operator()(const VariantKey& key) const
    {
        return std::hash<int>()(key.weight * 2 + (key.italic ? 1 : 0));
    }
};

// A CSS font for one face at the request's pixel size, and what has been
// measured of it.
struct WebVariant
{
    std::string font;
    FontMetrics metrics;
    std::unordered_map<char32_t, float> advances;
};
} // namespace

struct GlyphRasterizer::Native
{
    explicit Native(const FontRequest& requestToUse)
        : request(requestToUse)
    {
        webTextEnsureContext();
        resolve();
        valid = request.pixelSize() > 0.f && variant({}) != nullptr;
    }

    void resolve()
    {
        if (isMonospaceFamily(request.family))
        {
            family = "monospace";
            resolved = "monospace";
            return;
        }

        if (isGenericFamily(request.family))
        {
            family = request.family.empty() || request.family == "default"
                         ? "sans-serif"
                         : request.family;
            resolved = family;
            return;
        }

        if (!webTextHasFamily(request.family.c_str()))
        {
            family = "sans-serif";
            resolved = "sans-serif";
            return;
        }

        auto quoted = std::string {"\""};

        for (auto c: request.family)
        {
            if (c == '"' || c == '\\')
                quoted += '\\';

            quoted += c;
        }

        family = quoted + "\", sans-serif";
        resolved = request.family;
    }

    WebVariant* variant(const FontVariant& wanted) const
    {
        const auto key = VariantKey {weightClass(wanted.weight), wanted.italic};

        if (auto found = variants.find(key); found != variants.end())
            return &found->second;

        auto face = WebVariant {};
        face.font = std::string {key.italic ? "italic " : ""}
                    + std::to_string(key.weight) + " "
                    + std::to_string(request.pixelSize()) + "px " + family;

        face.metrics.ascent = (float) webTextFontMetric(face.font.c_str(), 0);
        face.metrics.descent = (float) webTextFontMetric(face.font.c_str(), 1);
        face.metrics.leading = 0.f;
        face.metrics.advance = (float) webTextFontMetric(face.font.c_str(), 2);

        return &variants.emplace(key, std::move(face)).first->second;
    }

    float advanceOf(WebVariant& face, char32_t codepoint) const
    {
        if (auto found = face.advances.find(codepoint); found != face.advances.end())
            return found->second;

        auto advance =
            (float) webTextWidth(face.font.c_str(), utf8Of(codepoint).c_str());

        face.advances.emplace(codepoint, advance);

        return advance;
    }

    FontMetrics metrics(const FontVariant& wanted) const
    {
        auto* face = valid ? variant(wanted) : nullptr;
        return face != nullptr ? face->metrics : FontMetrics {};
    }

    ShapedRun shape(std::string_view text, const FontVariant& wanted) const
    {
        auto result = ShapedRun {};
        auto* face = valid ? variant(wanted) : nullptr;

        if (face == nullptr)
            return result;

        auto index = 0;

        while (index < (int) text.size())
        {
            const auto cluster = index;
            const auto codepoint = decodeUtf8(text, index);

            result.glyphs.add(ShapedGlyph {
                {(std::uint32_t) codepoint, 0}, result.advance, 0.f, cluster});
            result.advance += advanceOf(*face, codepoint);
        }

        return result;
    }

    GlyphBitmap rasterize(GlyphKey key,
                          const FontVariant& wanted,
                          const RasterRequest& raster) const
    {
        auto result = GlyphBitmap {};
        auto* face = valid ? variant(wanted) : nullptr;

        if (face == nullptr || key.font != 0)
            return result;

        const auto codepoint = (char32_t) key.glyph;
        const auto text = utf8Of(codepoint);
        const auto* font = face->font.c_str();

        result.valid = true;
        result.advance = advanceOf(*face, codepoint);

        const auto inkLeft =
            (int) std::floor(webTextInkBound(font, text.c_str(), 0));
        const auto inkTop = (int) std::floor(webTextInkBound(font, text.c_str(), 1));
        const auto inkRight =
            (int) std::ceil(webTextInkBound(font, text.c_str(), 2));
        const auto inkBottom =
            (int) std::ceil(webTextInkBound(font, text.c_str(), 3));

        if (inkRight <= inkLeft || inkBottom <= inkTop)
            return result;

        // A pixel of room each way for the antialiased edge, and one more on
        // the right for the subpixel shift.
        const auto left = inkLeft - 1;
        const auto top = inkTop - 1;
        const auto width = inkRight + 2 - left;
        const auto height = inkBottom + 1 - top;

        result.width = width;
        result.height = height;
        result.format = GlyphFormat::Mask;
        result.pixels.resize(width * height);

        webTextDraw(font,
                    text.c_str(),
                    width,
                    height,
                    (double) (raster.subpixelX - (float) left),
                    (double) -top,
                    result.pixels.data());

        result.bearingX = (float) left;
        result.bearingY = (float) -top;

        return result;
    }

    FontRequest request;
    std::string family;
    std::string resolved;
    bool valid = false;

    mutable std::unordered_map<VariantKey, WebVariant, VariantKeyHash> variants;
};

GlyphRasterizer::GlyphRasterizer(const FontRequest& request)
    : impl(request)
{
}

GlyphRasterizer::~GlyphRasterizer() = default;

bool GlyphRasterizer::isValid() const
{
    return impl->valid;
}

std::string GlyphRasterizer::resolvedFamily() const
{
    return impl->resolved;
}

FontMetrics GlyphRasterizer::metrics(const FontVariant& variant) const
{
    return impl->metrics(variant);
}

float GlyphRasterizer::scale() const
{
    return impl->request.scale;
}

ShapedRun GlyphRasterizer::shape(std::string_view text,
                                 const FontVariant& variant) const
{
    return impl->shape(text, variant);
}

GlyphBitmap GlyphRasterizer::rasterize(GlyphKey glyph,
                                       const FontVariant& variant,
                                       const RasterRequest& request) const
{
    return impl->rasterize(glyph, variant, request);
}

GlyphBitmap GlyphRasterizer::rasterize(char32_t codepoint, FontStyle style) const
{
    return impl->rasterize({(std::uint32_t) codepoint, 0}, variantOf(style), {});
}

const FontRequest& GlyphRasterizer::request() const
{
    return impl->request;
}

// Not yet on the web: a FontFace loads from bytes through a promise, which a
// call that must answer now cannot wait on.
std::optional<RegisteredFont> registerMemoryFont(const void*, int)
{
    return std::nullopt;
}
} // namespace eacp::Text
