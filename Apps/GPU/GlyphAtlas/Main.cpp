#include <eacp/Graphics/Graphics.h>
#include <eacp/Sprites/Sprites.h>
#include <eacp/Text/Text.h>

#include <optional>
#include <string>
#include <vector>

// Text drawn from a glyph atlas: rasterize on demand, cache, upload only what
// changed, then draw each glyph as a textured quad.
//
// The layout here is the platform's: each line is shaped through the atlas,
// so "AV" is kerned, "fi" is one glyph and a weight the family has is that
// face, and every glyph is placed by its own bearings — which is what the
// atlas gained over the fixed-cell version it grew out of. Proportional text,
// a monospace block, and mixed styles all come out of the same path.
//
// Press a key to append it; the atlas rasterizes anything new on the spot.

using namespace eacp;

namespace
{
constexpr auto background = Graphics::Color {0.09f, 0.10f, 0.13f};
constexpr auto gutter = Graphics::Color {1.f, 1.f, 1.f, 0.04f};

struct Line
{
    std::string text;
    Text::FontStyle style = Text::FontStyle::Regular;
    Graphics::Color color = Graphics::Color::gray(0.86f);

    // Drawn in the proportional face rather than the monospace one, at a
    // CSS weight of its own when one is given.
    bool proportional = false;
    int weight = 0;

    Text::FontVariant variant() const
    {
        return {weight > 0 ? weight : (Text::isBold(style) ? 700 : 400),
                Text::isItalic(style)};
    }
};

// A proportional family with kerning pairs, ligatures and more than two
// weights, so the shaping shows.
constexpr const char* proportionalFamily()
{
#if defined(_WIN32)
    return "Segoe UI";
#else
    return "Helvetica Neue";
#endif
}

struct AtlasTextView final : GPU::GPUView
{
    AtlasTextView()
    {
        setSampleCount(1);
        setHandlesMouseEvents(true);

        lines = {
            {"The quick brown fox jumps over the lazy dog",
             Text::FontStyle::Regular},
            {"Bold text puts down more ink", Text::FontStyle::Bold},
            {"Italic leans, and shares the same atlas", Text::FontStyle::Italic},
            {"AVAST To fi fl - kerned and ligated, proportional",
             Text::FontStyle::Regular,
             Graphics::Color {0.95f, 0.85f, 0.55f},
             true},
            {"Light 300",
             Text::FontStyle::Regular,
             Graphics::Color::gray(0.86f),
             true,
             300},
            {"Regular 400  Bold 700  Black 900",
             Text::FontStyle::Regular,
             Graphics::Color::gray(0.86f),
             true,
             900},
            {"if (glyph.valid) { draw(glyph); }",
             Text::FontStyle::Regular,
             Graphics::Color {0.55f, 0.80f, 0.60f}},
            {"iiiii WWWWW .... — proportional advances",
             Text::FontStyle::Regular,
             Graphics::Color {0.85f, 0.65f, 0.45f}},
            {"Type to add glyphs: ",
             Text::FontStyle::Bold,
             Graphics::Color {0.55f, 0.70f, 0.95f}},
        };
    }

    // The atlas has to rasterize at the display's real scale, so it is built
    // once the view knows what that is — and rebuilt if the window moves to a
    // display with a different one.
    void ensureAtlas()
    {
        const auto scale = backingScale();

        if (atlas && builtAtScale == scale)
            return;

        auto request = Text::FontRequest {};
        request.family = Text::defaultMonospaceFamily();
        request.pointSize = 22.f;
        request.scale = scale;

        // A display change is the atlas's own business now: it rebuilds each
        // face at the new scale and drops what was rasterized for the old one,
        // which is what replacing the whole object used to do from out here.
        if (atlas)
        {
            atlas->setScale(scale);
            builtAtScale = scale;
            return;
        }

        if (!Text::GlyphRasterizer {request}.isValid())
            return;

        atlas = makeOwned<Text::GlyphAtlas>(
            Text::rasterizerFaceFactory(), request, 256, 2048);
        proportionalFace = atlas->findOrAddFace(proportionalFamily(), 22.f);

        builtAtScale = scale;
    }

    void resized() override
    {
        GPUView::resized();

        const auto bounds = getLocalBounds();

        if (bounds.w > 0 && bounds.h > 0)
        {
            // Both renderers take the new size the same cheap way: it is a
            // uniform on each, not anything either of them compiled.
            if (sprites)
                sprites->setLogicalSize({bounds.w, bounds.h});
            else
                sprites.emplace(Graphics::Point {bounds.w, bounds.h}, sampleCount());

            if (!glyphs)
                glyphs.emplace();

            glyphs->setViewportSize({bounds.w, bounds.h});
        }

        repaint();
    }

    void backingScaleChanged() override
    {
        GPUView::backingScaleChanged();

        // Glyphs cached for the old display are the wrong size now.
        ensureAtlas();
        repaint();
    }

    void keyDown(const Graphics::KeyEvent& event) override
    {
        if (event.characters.empty())
            return;

        auto& typed = lines.back().text;

        if (event.keyCode == Graphics::KeyCode::Delete)
        {
            // Trim a whole UTF-8 sequence, not a byte.
            while (typed.size() > prefixLength)
            {
                const auto last = (unsigned char) typed.back();
                typed.pop_back();

                if ((last & 0xc0) != 0x80)
                    break;
            }
        }
        else
        {
            typed += event.characters;
        }

        repaint();
    }

    // Shapes one line through the atlas and places each glyph by its own
    // bearings. Returns the pen position it ended at.
    float layOutLine(const Line& line, float x, float baseline, bool collectOnly)
    {
        const auto face = line.proportional ? proportionalFace : 0;
        const auto shaped = atlas->shape(line.text, line.variant(), face);

        if (collectOnly)
            return x + shaped.advance;

        for (const auto& placed: shaped.glyphs)
        {
            const auto& glyph = placed.slot;

            if (!glyph.valid || glyph.empty)
                continue;

            // The glyph's pen and the slot's offset are measured from the
            // line's pen and the baseline, so this is the destination rect's
            // top-left directly.
            const auto destination =
                Graphics::Rect {x + placed.pen.x + glyph.offset.x,
                                baseline + placed.pen.y + glyph.offset.y,
                                glyph.src.w / builtAtScale,
                                glyph.src.h / builtAtScale};

            // Masks carry coverage only and take the line's colour; colour
            // glyphs carry their own and are drawn untinted. GlyphRenderer
            // keeps the two in separate queues and shades each correctly — a
            // general sprite shader would multiply the mask's coverage into
            // RGB and draw opaque red boxes instead of text.
            const auto colored = glyph.format == Text::GlyphFormat::Color;

            glyphs->add(destination, glyph.src, line.color, colored);
        }

        return x + shaped.advance;
    }

    void render(GPU::Frame& frame) override
    {
        ensureAtlas();

        auto pass = frame.beginPass({background});

        if (!sprites || !glyphs || !atlas)
            return;

        const auto metrics = atlas->metrics();
        const auto lineHeight = metrics.lineHeight() * 1.35f;
        const auto left = 32.f;
        auto baseline = 56.f + metrics.ascent;

        // Every glyph the frame needs is requested before the first draw, then
        // committed once. Uploading mid-pass would mutate a texture the earlier
        // draws have already bound.
        for (const auto& line: lines)
            layOutLine(line, left, baseline, true);

        atlas->commit();

        sprites->begin(pass);
        glyphs->begin();

        for (const auto& line: lines)
        {
            sprites->fillRect(
                {0.f, baseline - metrics.ascent, getLocalBounds().w, lineHeight},
                &line == &lines.back() ? gutter : Graphics::Color {0, 0, 0, 0});

            layOutLine(line, left, baseline, false);
            baseline += lineHeight;
        }

        // The gutter fills are queued rather than drawn, and the pass would not
        // collect them until it ends - by which time the glyphs below would
        // already be under them. This is the one thing batching still asks of a
        // caller: say when, if the order matters before the pass is over.
        sprites->flush();

        // Every glyph in one or two draw calls, issued after the gutter fills so
        // the text lands on top of them.
        glyphs->flush(pass, *atlas);
    }

    std::optional<Sprites::SpriteRenderer> sprites;
    std::optional<Text::GlyphRenderer> glyphs;
    OwningPointer<Text::GlyphAtlas> atlas;
    int proportionalFace = 0;
    float builtAtScale = 0.f;

    std::vector<Line> lines;
    std::size_t prefixLength = std::string("Type to add glyphs: ").size();
};

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};

    options.width = 820;
    options.height = 420;
    options.minWidth = 360;
    options.minHeight = 200;
    options.title = "Glyph Atlas";
    options.backgroundColor = background;

    return options;
}

struct GlyphAtlasApp
{
    GlyphAtlasApp() { window.setContentView(view); }

    AtlasTextView view;
    Graphics::Window window {windowOptions()};
};
} // namespace

int main()
{
    return eacp::Apps::run<GlyphAtlasApp>();
}
