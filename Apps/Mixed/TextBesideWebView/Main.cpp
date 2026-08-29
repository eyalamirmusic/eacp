#include <eacp/UI/UI.h>
#include <eacp/WebView/WebView.h>

#include <cmath>
#include <iterator>
#include <string>

// The same lines of text twice: on the left the system web view -- the
// browser engine drawing through the platform's own text stack, which is the
// text the rest of the system is judged against -- and on the right the
// component tier's TextRenderer, at the same families, sizes, weights and
// positions. The right half is there to be read against the left, and the
// difference should be none the eye can find: each glyph is rasterized with
// the platform's font smoothing and at the quarter of a pixel its pen fell in,
// then placed on a whole device pixel, which is what the engine's own painter
// does.
//
// The rows are one table and the page on the left is generated from it, so
// the halves cannot drift apart. Each row is positioned absolutely with
// `line-height: normal`, which puts the engine's baseline at the row's top plus
// the face's ascent, rounded, plus half its line gap; the right half puts its
// baseline at the top plus the ascent, rounded. For a face with a line gap the
// two differ by a pixel, which the comparison tolerates: it is about the
// glyphs, not the layout. The rows' tops accumulate to fractions on purpose --
// a fractional baseline is the case that used to blur a whole line.
//
// The last row is the web view's `-webkit-font-smoothing: antialiased`, the
// thin rendering the tier used to have. Beside the row above it, that is the
// sixth of the ink the smoothing adds to dark text -- it adds a third to
// light text, which is why a mask is rasterized in the lightness of the text
// it is for, and why this page is dark on light: the common case, and the one
// the tier used to draw a sixth too heavy.

using namespace eacp;

namespace
{
constexpr auto leftInset = 24.f;
constexpr auto captionTop = 14.f;
constexpr auto firstRowTop = 48.f;

// The platform's own UI and monospace faces, never a literal family name: a
// name only one system ships resolves to a substitute on the other, and the two
// halves pick different substitutes -- the web engine's default standard face
// against the rasterizer's -- so the comparison would be between two faces
// rather than between two renderings of one. On Windows that reads as a size
// difference, since the engine's fallback is a serif with a much smaller
// x-height than Segoe UI. Asking for the family the platform actually has puts
// both halves in it.
constexpr auto ui = UI::defaultUIFontFamily();
constexpr auto mono = Text::defaultMonospaceFamily();
constexpr auto fox = "The quick brown fox jumps over the lazy dog 0123456789";
constexpr auto sphinx = "Sphinx of black quartz, judge my vow.";
constexpr auto kerned = "AVAWATo Ty fi fl: kerned pairs and ligatures";
constexpr auto jugs = "Pack my box with five dozen liquor jugs.";
constexpr auto code = "for (auto& glyph: run.glyphs) place(glyph);";
constexpr auto waltz = "Waltz, bad nymph, for quick jigs vex";
constexpr auto thinNote = "the weight the tier used to draw";

struct Row
{
    const char* family;
    float size;
    int weight;
    bool italic;
    bool thin;
    const char* text;
};

constexpr Row rows[] = {
    {ui, 11.f, 400, false, false, fox},
    {ui, 13.f, 400, false, false, fox},
    {ui, 16.f, 400, false, false, kerned},
    {ui, 16.f, 700, false, false, sphinx},
    {ui, 16.f, 400, true, false, sphinx},
    {ui, 16.f, 300, false, false, sphinx},
    {"Georgia", 16.f, 400, false, false, jugs},
    {mono, 13.f, 400, false, false, code},
    {ui, 24.f, 400, false, false, waltz},
    {ui, 40.f, 700, false, false, "Hamburgefonstiv"},
    {ui, 16.f, 400, false, true, thinNote},
};

constexpr auto captionSize = 13.f;
constexpr auto captionWeight = 600;

// Where each row's top sits. The pitch is a fraction of the size, so the tops
// land on fractions of a pixel after the first few rows.
float rowTop(int index)
{
    auto top = firstRowTop;

    for (auto i = 0; i < index; ++i)
        top += rows[i].size * 1.7f + 6.f;

    return top;
}

std::string pixels(float value)
{
    auto text = std::to_string(value);

    // to_string writes six decimals; the fraction matters, the trailing zeros
    // do not.
    while (text.size() > 1 && text.back() == '0')
        text.pop_back();

    if (text.back() == '.')
        text.pop_back();

    return text + "px";
}

// What the row says it is, ahead of its text: "16px bold", "Georgia 16px".
std::string labelled(const Row& row)
{
    auto label = std::string {row.family == ui ? "" : row.family};

    if (!label.empty())
        label += ' ';

    label += pixels(row.size);

    if (row.weight == 700)
        label += " bold";
    else if (row.weight == 300)
        label += " light";

    if (row.italic)
        label += " italic";

    if (row.thin)
        label += " antialiased:";

    return label + ' ' + row.text;
}

std::string escaped(std::string_view text)
{
    auto result = std::string {};

    for (const auto character: text)
    {
        if (character == '&')
            result += "&amp;";
        else if (character == '<')
            result += "&lt;";
        else
            result += character;
    }

    return result;
}

std::string cssFont(const char* family, float size, int weight, bool italic)
{
    return std::string {italic ? "italic " : ""} + std::to_string(weight) + " "
           + pixels(size) + " '" + family + "'";
}

std::string placed(const std::string& classes,
                   float top,
                   const std::string& font,
                   const std::string& text)
{
    return "<div class=\"" + classes + "\" style=\"left:" + pixels(leftInset)
           + ";top:" + pixels(top) + ";font:" + font + "\">" + escaped(text)
           + "</div>\n";
}

std::string specimenHtml(const std::string& caption)
{
    auto html =
        std::string {R"HTML(<!doctype html><html><head><meta charset="utf-8"><style>
  html, body { margin: 0; background: #ffffff; color: #1a1a1a; overflow: hidden; }
  div { position: absolute; margin: 0; white-space: nowrap; line-height: normal; }
  .caption { color: #6b6b6b; }
  .thin { -webkit-font-smoothing: antialiased; }
</style></head><body>
)HTML"};

    html += placed("caption",
                   captionTop,
                   cssFont(ui, captionSize, captionWeight, false),
                   caption);

    for (auto i = 0; i < (int) std::size(rows); ++i)
    {
        const auto& row = rows[i];

        html += placed(row.thin ? "thin" : "",
                       rowTop(i),
                       cssFont(row.family, row.size, row.weight, row.italic),
                       labelled(row));
    }

    return html + "</body></html>";
}

UI::Font fontFor(const char* family, float size, int weight, bool italic)
{
    auto font = UI::Font {};
    font.family = family;
    font.pointSize = size;
    font.style = italic ? UI::FontStyle::Italic : UI::FontStyle::Regular;
    font.weight = weight;

    return font;
}

// The right half: every row drawn with its pen where the engine puts its
// baseline, the ascent rounded as the engine rounds it.
struct Specimen final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        drawRow(g,
                fontFor(ui, captionSize, captionWeight, false),
                captionTop,
                "eacp TextRenderer",
                UI::Color {0.42f, 0.42f, 0.42f, 1.f});

        for (auto i = 0; i < (int) std::size(rows); ++i)
        {
            const auto& row = rows[i];

            drawRow(g,
                    fontFor(row.family, row.size, row.weight, row.italic),
                    rowTop(i),
                    labelled(row),
                    UI::Color {0.102f, 0.102f, 0.102f, 1.f});
        }
    }

    static void drawRow(UI::Graphics& g,
                        const UI::Font& font,
                        float top,
                        std::string_view text,
                        const UI::Color& colour)
    {
        g.setFont(font);
        g.setColour(colour);
        g.drawText(text, {leftInset, top + std::round(g.ascent())});
    }
};

struct SpecimenHost final : UI::ComponentHost
{
    SpecimenHost()
    {
        setBackgroundColour(UI::Color::white());
        setRootComponent(specimen);
    }

    Specimen specimen;
};

Graphics::WebView::Options webViewOptions()
{
    auto options = Graphics::WebView::Options {};
    options.statusBar = false;

    return options;
}

// The two tiers as siblings in one native view, the web view on the left and
// the GPU view holding the component tree on the right.
struct SplitView final : Graphics::View
{
    SplitView()
    {
        addChildren({webView, host});
        webView.loadHTML(specimenHtml("System web view"), "https://localhost/");
    }

    void resized() override
    {
        auto area = getLocalBounds();

        webView.setBounds(area.removeFromLeft(area.w * 0.5f));
        host.setBounds(area);
    }

    Graphics::WebView webView {webViewOptions()};
    SpecimenHost host;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.title = "eacp Mixed — system web view | TextRenderer";
    options.width = 1360;
    options.height = 640;
    options.minWidth = 800;
    options.minHeight = 400;

    return options;
}

struct App
{
    App() { window.setContentView(split); }

    SplitView split;
    Graphics::Window window {makeOptions()};
};
} // namespace

int main()
{
    return Apps::run<App>();
}
