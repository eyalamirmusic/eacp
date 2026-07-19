#include <eacp/Graphics/Graphics.h>
#include <eacp/Sprites/Sprites.h>
#include <eacp/Text/Text.h>

#include <optional>
#include <string>
#include <vector>

// Text drawn from a glyph atlas: rasterize on demand, cache, upload only what
// changed, then draw each glyph as a textured quad.
//
// The layout here is a real one, not a grid — each glyph is placed by its own
// bearings and steps the pen by its own advance, which is what the atlas gained
// over the fixed-cell version it grew out of. Proportional text, a monospace
// block, and mixed styles all come out of the same path.
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
};

// Minimal UTF-8 decode, enough to walk the sample text and typed input.
char32_t nextCodepoint(const std::string& text, std::size_t& index)
{
    const auto lead = (unsigned char) text[index];

    if (lead < 0x80)
        return (char32_t) text[index++];

    auto extra = 0;
    auto value = char32_t {0};

    if ((lead & 0xe0) == 0xc0)
    {
        extra = 1;
        value = lead & 0x1fu;
    }
    else if ((lead & 0xf0) == 0xe0)
    {
        extra = 2;
        value = lead & 0x0fu;
    }
    else
    {
        extra = 3;
        value = lead & 0x07u;
    }

    ++index;

    for (auto i = 0; i < extra && index < text.size(); ++i, ++index)
        value = (value << 6) | ((unsigned char) text[index] & 0x3fu);

    return value;
}

struct AtlasTextView final : GPU::GPUView
{
    AtlasTextView()
    {
        setSampleCount(1);
        setHandlesMouseEvents(true);

        lines = {
            {"The quick brown fox jumps over the lazy dog", Text::FontStyle::Regular},
            {"Bold text puts down more ink", Text::FontStyle::Bold},
            {"Italic leans, and shares the same atlas", Text::FontStyle::Italic},
            {"if (glyph.valid) { draw(glyph); }", Text::FontStyle::Regular,
             Graphics::Color {0.55f, 0.80f, 0.60f}},
            {"iiiii WWWWW .... — proportional advances", Text::FontStyle::Regular,
             Graphics::Color {0.85f, 0.65f, 0.45f}},
            {"Type to add glyphs: ", Text::FontStyle::Bold,
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

        auto rasterizer = makeOwned<Text::GlyphRasterizer>(request);

        if (!rasterizer->isValid())
            return;

        atlas.reset();
        atlas = makeOwned<Text::GlyphAtlas>(
            OwningPointer<Text::GlyphSource> {std::move(rasterizer)}, 256, 2048);

        builtAtScale = scale;
    }

    void resized() override
    {
        GPUView::resized();

        const auto bounds = getLocalBounds();

        if (bounds.w > 0 && bounds.h > 0)
        {
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

    // Walks one line, asking the atlas for each glyph and placing it by its own
    // bearings. Returns the pen position it ended at.
    float layOutLine(const Line& line, float x, float baseline, bool collectOnly)
    {
        auto pen = x;

        for (std::size_t index = 0; index < line.text.size();)
        {
            const auto codepoint = nextCodepoint(line.text, index);
            const auto glyph = atlas->glyph(codepoint, line.style);

            if (!glyph.valid)
                continue;

            if (!glyph.empty && !collectOnly)
            {
                // offset is measured from the pen and the baseline, so this is
                // the destination rect's top-left directly.
                const auto destination = Graphics::Rect {pen + glyph.offset.x,
                                                         baseline + glyph.offset.y,
                                                         glyph.src.w / builtAtScale,
                                                         glyph.src.h / builtAtScale};

                // Masks carry coverage only and take the line's colour; colour
                // glyphs carry their own and are drawn untinted. GlyphRenderer
                // keeps the two in separate queues and shades each correctly —
                // a general sprite shader would multiply the mask's coverage
                // into RGB and draw opaque red boxes instead of text.
                const auto colored = glyph.format == Text::GlyphFormat::Color;

                glyphs->add(destination, glyph.src, line.color, colored);
            }

            pen += glyph.advance;
        }

        return pen;
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
            sprites->fillRect({0.f, baseline - metrics.ascent, getLocalBounds().w, lineHeight},
                              &line == &lines.back() ? gutter : Graphics::Color {0, 0, 0, 0});

            layOutLine(line, left, baseline, false);
            baseline += lineHeight;
        }

        // Every glyph in one or two draw calls, issued after the gutter fills so
        // the text lands on top of them.
        glyphs->flush(pass, *atlas);
    }

    std::optional<Sprites::SpriteRenderer> sprites;
    std::optional<Text::GlyphRenderer> glyphs;
    OwningPointer<Text::GlyphAtlas> atlas;
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
