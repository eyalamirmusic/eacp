#include <eacp/Graphics/Graphics.h>
#include <eacp/Sprites/Sprites.h>
#include <eacp/Text/Text.h>

#include <ResEmbed/ResEmbed.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// A variable font, and what one is to the text tier.
//
// A family is normally cut into faces: a file per weight, and a request for
// 600 is answered by picking the file nearest it. A family has a handful of
// them, so nine weights come out as a handful of thicknesses with repeats
// between — which is the right column here. A variable font is one file with
// an axis instead: Inter's `wght` runs from 100 to 900 continuously, and a
// weight is a value set on the face rather than a sibling file to look for.
// That is the left column: nine rows, one file, no two the same.
//
// None of that reaches a caller. The whole of it is
// Text::registerMemoryFont(bytes, size) — which copies the bytes and hands
// back the family the platform filed the face under, not necessarily the name
// the file was known by — and then Text::Font::weight, or the FontVariant a
// shape() takes, on every request. The rasterizer asks the face's own axes
// only for what matching a face could not supply, and the atlas keys on the
// variant, so two weights of one file are two cache entries and not one.
//
// The left and right arrows (or - and =) move the live row's weight. In steps
// of a hundred, because Font::variant() rounds a weight to the nearest of
// CSS's nine — Font.h's weightClass — and the atlas keys on that: 610 and 650
// are one entry and one thickness, so a hundred is the smallest step the tier
// draws differently.
//
// Inter's other axis, `opsz`, is set for the caller and never asked about:
// the rasterizer pins it to the point size, so a specimen set at 22 points is
// Inter's 22-point design on any display, rather than its display design on a
// Retina panel and its text design off one.
//
// The font is embedded — Inter, subset to printable ASCII, SIL Open Font
// License 1.1, see OFL.txt beside this file — so nothing is installed and
// nothing is read from disk.

using namespace eacp;

namespace
{
constexpr auto background = Graphics::Color {0.09f, 0.10f, 0.13f};
constexpr auto rowFill = Graphics::Color {1.f, 1.f, 1.f, 0.035f};
constexpr auto liveRowFill = Graphics::Color {0.35f, 0.55f, 0.95f, 0.16f};
constexpr auto heading = Graphics::Color {0.55f, 0.70f, 0.95f};
constexpr auto note = Graphics::Color::gray(0.62f);
constexpr auto ink = Graphics::Color::gray(0.88f);
constexpr auto failure = Graphics::Color {0.95f, 0.55f, 0.45f};

constexpr auto specimen = "Hamburgefonstiv 0123456789";
constexpr auto specimenPointSize = 22.f;
constexpr auto notePointSize = 13.f;

constexpr auto margin = 28.f;
constexpr auto labelColumnWidth = 54.f;
constexpr auto columnGap = 40.f;

// The nine CSS weights, which are also the nine the atlas can tell apart.
constexpr int weights[] = {100, 200, 300, 400, 500, 600, 700, 800, 900};
constexpr auto weightStep = 100;

// A family cut into faces, so the two mechanisms sit side by side. No name
// ships on both systems.
constexpr const char* staticFamily()
{
#if defined(_WIN32)
    return "Segoe UI";
#else
    return "Helvetica Neue";
#endif
}

// The embedded face, registered with the platform on the first ask and kept:
// registering is process-wide and the same bytes twice are one face.
const std::optional<Text::RegisteredFont>& embeddedFont()
{
    static const auto registered = []() -> std::optional<Text::RegisteredFont>
    {
        const auto file =
            ResEmbed::get("InterVariable-Subset.ttf", "VariableFontAssets");

        if (!file)
            return std::nullopt;

        return Text::registerMemoryFont(file.asRaw(), file.size());
    }();

    return registered;
}

// One string of the frame, so that the walk which asks the atlas for glyphs
// and the walk which draws them are the same walk.
enum class Pass
{
    collect,
    draw
};

struct VariableFontView final : GPU::GPUView
{
    VariableFontView()
    {
        setSampleCount(1);
        setHandlesMouseEvents(true);

        if (const auto& registered = embeddedFont())
            notes = {{"registerMemoryFont: family \"" + registered->family
                          + "\", PostScript \"" + registered->postScriptName + "\"",
                      note},
                     {"One embedded file, nine weights: Font::weight is the whole "
                      "of the caller's side.",
                      note}};
        else
            notes = {{"registerMemoryFont refused the embedded bytes - no variable "
                      "face to show.",
                      failure},
                     {"The right-hand column is the platform's own family, which "
                      "needs no registering.",
                      note}};
    }

    // The atlas has to rasterize at the display's real scale, so it is built
    // once the view knows what that is - and told about a move to a display
    // with a different one.
    void ensureAtlas()
    {
        const auto scale = backingScale();

        if (atlas && builtAtScale == scale)
            return;

        auto request = Text::FontRequest {};
        request.family = Text::defaultMonospaceFamily();
        request.pointSize = notePointSize;
        request.scale = scale;

        if (atlas)
        {
            atlas->setScale(scale);
            builtAtScale = scale;
            return;
        }

        if (!Text::GlyphRasterizer {request}.isValid())
            return;

        atlas = makeOwned<Text::GlyphAtlas>(
            Text::rasterizerFaceFactory(), request, 512, 4096);

        if (const auto& registered = embeddedFont())
            variableFace =
                atlas->findOrAddFace(registered->family, specimenPointSize);

        staticFace = atlas->findOrAddFace(staticFamily(), specimenPointSize);
        builtAtScale = scale;

        measureColumns();
    }

    // The specimen is not the same width at every weight or in either family,
    // so the second column starts past the widest any row will be.
    void measureColumns()
    {
        auto widest = 0.f;

        for (const auto weight: weights)
        {
            const auto variant = Text::FontVariant {weight, false};

            if (variableFace >= 0)
                widest = std::max(
                    widest, atlas->shape(specimen, variant, variableFace).advance);

            widest = std::max(widest,
                              atlas->shape(specimen, variant, staticFace).advance);
        }

        specimenColumn = margin + labelColumnWidth;
        staticColumn = specimenColumn + widest + columnGap;
    }

    void resized() override
    {
        GPUView::resized();

        const auto bounds = getLocalBounds();

        if (bounds.w > 0 && bounds.h > 0)
        {
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

        ensureAtlas();
        repaint();
    }

    void keyDown(const Graphics::KeyEvent& event) override
    {
        const auto code = event.keyCode;

        if (code == Graphics::KeyCode::RightArrow
            || code == Graphics::KeyCode::Equals)
            setLiveWeight(liveWeight + weightStep);
        else if (code == Graphics::KeyCode::LeftArrow
                 || code == Graphics::KeyCode::Minus)
            setLiveWeight(liveWeight - weightStep);
    }

    void setLiveWeight(int weight)
    {
        liveWeight = std::clamp(weight, weights[0], weights[std::size(weights) - 1]);
        repaint();
    }

    float widthOf(std::string_view text, int face, const Text::FontVariant& variant)
    {
        return atlas->shape(text, variant, face).advance;
    }

    // Shapes one string through the atlas and places each glyph by its own
    // bearings. Returns the pen position it ended at.
    float drawText(std::string_view text,
                   int face,
                   const Text::FontVariant& variant,
                   float x,
                   float baseline,
                   const Graphics::Color& color,
                   Pass pass)
    {
        const auto shaped = atlas->shape(text, variant, face);

        if (pass == Pass::collect)
            return x + shaped.advance;

        for (const auto& placed: shaped.glyphs)
        {
            const auto& glyph = placed.slot;

            if (!glyph.valid || glyph.empty)
                continue;

            const auto destination =
                Graphics::Rect {x + placed.pen.x + glyph.offset.x,
                                baseline + placed.pen.y + glyph.offset.y,
                                glyph.src.w / builtAtScale,
                                glyph.src.h / builtAtScale};

            glyphs->add(destination,
                        glyph.src,
                        color,
                        glyph.format == Text::GlyphFormat::Color);
        }

        return x + shaped.advance;
    }

    void drawRow(int weight, float top, float rowHeight, bool live, Pass pass)
    {
        const auto variant = Text::FontVariant {weight, false};
        const auto metrics = atlas->metrics(variant, staticFace);
        const auto baseline =
            top + (rowHeight - metrics.lineHeight()) * 0.5f + metrics.ascent;

        if (pass == Pass::draw)
            sprites->fillRect({0.f, top, getLocalBounds().w, rowHeight},
                              live ? liveRowFill : rowFill);

        const auto label = std::to_string(weight);
        const auto labelVariant = Text::FontVariant {};
        const auto labelRight = margin + labelColumnWidth - columnGap * 0.4f;

        drawText(label,
                 labelFace,
                 labelVariant,
                 labelRight - widthOf(label, labelFace, labelVariant),
                 baseline,
                 live ? heading : note,
                 pass);

        if (variableFace >= 0)
            drawText(specimen,
                     variableFace,
                     variant,
                     specimenColumn,
                     baseline,
                     ink,
                     pass);

        drawText(specimen, staticFace, variant, staticColumn, baseline, ink, pass);
    }

    void layOutFrame(Pass pass)
    {
        const auto noteMetrics = atlas->metrics(Text::FontVariant {}, labelFace);
        const auto noteHeight = noteMetrics.lineHeight() * 1.5f;
        const auto rowHeight =
            atlas->metrics(Text::FontVariant {}, staticFace).lineHeight() * 1.4f;

        auto y = margin;

        for (const auto& line: notes)
        {
            drawText(line.text,
                     labelFace,
                     {},
                     margin,
                     y + noteMetrics.ascent,
                     line.color,
                     pass);
            y += noteHeight;
        }

        y += noteHeight * 0.5f;

        const auto& registered = embeddedFont();
        const auto variableHeading =
            registered ? registered->family + " - one file, wght 100 to 900"
                       : std::string {"(not registered)"};

        drawText(variableHeading,
                 labelFace,
                 {},
                 specimenColumn,
                 y + noteMetrics.ascent,
                 heading,
                 pass);
        drawText(std::string {staticFamily()} + " - the faces the family has",
                 labelFace,
                 {},
                 staticColumn,
                 y + noteMetrics.ascent,
                 heading,
                 pass);

        y += noteHeight;

        for (const auto weight: weights)
        {
            drawRow(weight, y, rowHeight, false, pass);
            y += rowHeight;
        }

        y += noteHeight * 0.5f;

        drawText("Live row - left and right arrows, or - and =, move it by 100",
                 labelFace,
                 {},
                 specimenColumn,
                 y + noteMetrics.ascent,
                 heading,
                 pass);

        y += noteHeight;

        drawRow(liveWeight, y, rowHeight, true, pass);
    }

    void render(GPU::Frame& frame) override
    {
        ensureAtlas();

        auto pass = frame.beginPass({background});

        if (!sprites || !glyphs || !atlas)
            return;

        // Every glyph the frame needs is requested before the first draw, then
        // committed once. Uploading mid-pass would mutate a texture the earlier
        // draws have already bound.
        layOutFrame(Pass::collect);
        atlas->commit();

        sprites->begin(pass);
        glyphs->begin();

        layOutFrame(Pass::draw);

        // The stripes are queued rather than drawn, and the pass would not
        // collect them until it ends - by which time the text would already be
        // under them.
        sprites->flush();
        glyphs->flush(pass, *atlas);
    }

    struct Note
    {
        std::string text;
        Graphics::Color color;
    };

    std::optional<Sprites::SpriteRenderer> sprites;
    std::optional<Text::GlyphRenderer> glyphs;
    OwningPointer<Text::GlyphAtlas> atlas;

    static constexpr int labelFace = 0;
    int variableFace = -1;
    int staticFace = 0;
    float builtAtScale = 0.f;

    float specimenColumn = margin + labelColumnWidth;
    float staticColumn = 0.f;

    std::vector<Note> notes;
    int liveWeight = 400;
};

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};

    options.width = 880;
    options.height = 545;
    options.minWidth = 480;
    options.minHeight = 320;
    options.title = "Variable Font";
    options.backgroundColor = background;

    return options;
}

struct VariableFontApp
{
    VariableFontApp() { window.setContentView(view); }

    VariableFontView view;
    Graphics::Window window {windowOptions()};
};
} // namespace

int main()
{
    return eacp::Apps::run<VariableFontApp>();
}
