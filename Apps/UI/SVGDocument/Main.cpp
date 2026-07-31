#include <eacp/SVG/SVG.h>
#include <eacp/UI/UI.h>

#include <cmath>
#include <string>

// The same SVG document drawn twice: on the left by the native builder, on the
// right through the component tier.
//
// The left half is one Graphics::ShapeLayer per shape -- a CAShapeLayer on
// macOS, a Direct2D geometry on Windows -- which is what eacp-svg has always
// done. The right half is one UI::PathShape per shape: every mask in the
// document rasterized by a single compute dispatch before the frame opens, and
// then drawn as quads out of one shared atlas, in one instanced draw, alongside
// whatever else the interface is drawing.
//
// Side by side because the two questions this rung exists to answer are both
// questions you answer by looking.
//
// The first is whether a document fits in the atlas at all. A mask is the size
// of its shape on screen, and the argument that the atlas is therefore always
// big enough assumes shapes tile: everything visible at once cannot exceed the
// window's own area. Artwork does not tile, it stacks -- a background, then
// shapes over it, then shapes over those, each carrying its own full
// bounding-box mask -- and the sum of those is bounded by nothing. The Stacked
// document below is that case on purpose, and the footer reports how full the
// atlas is and how many masks were refused.
//
// The second is whether abutting shapes seam. Coverage accumulates within a path
// and not between draws, so two shapes sharing an edge are two draws with two
// antialiased edges and the background may show through the join. Widgets rarely
// abut; artwork does it constantly. The Tiles document is a grid of triangles
// sharing exact vertices, which is that case at its worst.

using namespace eacp;

namespace
{
constexpr auto padding = 12.f;

// A badge: a stroked and filled rounded rect, a circle, and text at a size the
// host's font is not. Small enough to check that the port draws the same picture
// before asking it anything harder.
const auto badgeDocument = std::string {
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
  <rect x="10" y="10" width="180" height="180" rx="20" fill="#4A90D9" stroke="#2C5F8A" stroke-width="3"/>
  <circle cx="100" cy="80" r="30" fill="#F5A623"/>
  <text x="100" y="160" text-anchor="middle" font-family="Helvetica" font-size="18" fill="white">eacp SVG</text>
</svg>)SVG"};

// Everything rung 1 added, in one document: a viewBox with a non-zero origin, a
// group whose fill its children inherit, transform lists that only compose
// correctly if they are matrices, an even-odd fill, round joins and caps, and
// text at three sizes.
const auto featureDocument = std::string {
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="360" height="260" viewBox="20 20 360 260">
  <rect x="20" y="20" width="360" height="260" fill="#FDFBF7"/>

  <g fill="#3B7A57" stroke="#204A34" stroke-width="2">
    <circle cx="80" cy="90" r="34"/>
    <rect x="130" y="56" width="68" height="68" rx="14"/>
    <polygon points="230,124 264,56 298,124"/>
  </g>

  <g transform="translate(300 190) rotate(-30)">
    <rect x="-46" y="-16" width="92" height="32" rx="16" fill="#D96A4A" stroke="#8C3A22" stroke-width="3" stroke-linejoin="round"/>
  </g>

  <path d="M 40 200 C 80 150, 140 250, 180 200 S 250 150, 280 200"
        fill="none" stroke="#2C5F8A" stroke-width="6" stroke-linecap="round"/>

  <path d="M 60 240 h 60 v 34 h -60 z M 75 252 h 30 v 12 h -30 z"
        fill="#7A4FA3" fill-rule="evenodd"/>

  <text x="200" y="164" text-anchor="middle" font-family="Helvetica" font-size="22" fill="#204A34">Component tier</text>
  <text x="200" y="182" text-anchor="middle" font-family="Helvetica" font-size="11" fill="#7A6A5A">one dispatch, one draw</text>
  <text x="376" y="272" text-anchor="end" font-family="Helvetica" font-size="9" fill="#B0A294">viewBox origin 20,20</text>
</svg>)SVG"};

// A grid of triangles sharing exact vertices, in alternating colours: the seam
// question at its worst, since every interior edge is drawn twice by two shapes
// that each antialias against nothing.
std::string makeTilesDocument(int columns, int rows)
{
    auto cell = 40;
    auto width = columns * cell;
    auto height = rows * cell;

    auto markup = std::string {R"(<svg xmlns="http://www.w3.org/2000/svg" width=")"}
                  + std::to_string(width) + R"(" height=")" + std::to_string(height)
                  + R"(" viewBox="0 0 )" + std::to_string(width) + " "
                  + std::to_string(height) + R"(">)";

    auto corner = [cell](int column, int row)
    { return std::to_string(column * cell) + "," + std::to_string(row * cell); };

    for (auto row = 0; row < rows; ++row)
    {
        for (auto column = 0; column < columns; ++column)
        {
            auto shade = (row * columns + column) % 5;
            auto colours = {"#4A90D9", "#3B7A57", "#D96A4A", "#7A4FA3", "#F5A623"};
            auto colour = *(colours.begin() + shade);

            markup += R"(<polygon points=")" + corner(column, row) + " "
                      + corner(column + 1, row) + " " + corner(column, row + 1)
                      + R"(" fill=")" + colour + R"("/>)";

            markup += R"(<polygon points=")" + corner(column + 1, row) + " "
                      + corner(column + 1, row + 1) + " " + corner(column, row + 1)
                      + R"(" fill=")"
                      + std::string(*(colours.begin() + (shade + 2) % 5)) + R"("/>)";
        }
    }

    return markup + "</svg>";
}

// Large shapes stacked on one another, which is the shape of real artwork and
// the case plan.md's atlas-ceiling argument does not cover. Each blob is about a
// third of the document across, so its mask is about a ninth of the document's
// area, and the sum runs past the window's own area many times over.
std::string makeStackedDocument(int shapeCount)
{
    auto size = 1000;

    auto markup = std::string {R"(<svg xmlns="http://www.w3.org/2000/svg" width=")"}
                  + std::to_string(size) + R"(" height=")" + std::to_string(size)
                  + R"(" viewBox="0 0 )" + std::to_string(size) + " "
                  + std::to_string(size) + R"(">)";

    markup += R"(<rect x="0" y="0" width=")" + std::to_string(size) + R"(" height=")"
              + std::to_string(size) + R"(" fill="#F7F4EF"/>)";

    for (auto index = 0; index < shapeCount; ++index)
    {
        // Spread over the document by two coprime steps, so consecutive shapes
        // land nowhere near each other and the packing is not doing anything
        // clever with locality.
        auto centreX = 120 + (index * 137) % (size - 240);
        auto centreY = 120 + (index * 211) % (size - 240);
        auto radius = 120 + (index * 37) % 90;

        auto shade = index % 6;
        auto colours = {
            "#4A90D9", "#3B7A57", "#D96A4A", "#7A4FA3", "#F5A623", "#2C5F8A"};

        markup += R"(<circle cx=")" + std::to_string(centreX) + R"(" cy=")"
                  + std::to_string(centreY) + R"(" r=")" + std::to_string(radius)
                  + R"(" fill=")" + std::string(*(colours.begin() + shade))
                  + R"(" opacity="0.55"/>)";
    }

    return markup + "</svg>";
}

struct Document
{
    std::string name;
    std::string markup;

    // Whether the native side draws it too.
    //
    // It cannot always. A native shape layer is a DirectComposition surface (a
    // CALayer on macOS) sized to the view, because the geometry is in the view's
    // coordinates and a native path cannot be translated -- so a document of 300
    // shapes asks the window server for 300 surfaces the size of the window, and
    // on this window that is gigabytes. The component tier draws the same
    // document into one shared atlas.
    //
    // Which is the comparison rather than a limitation of the demo, so it is
    // said on screen rather than worked around.
    bool nativeCanAfford = true;
};

Vector<Document> makeDocuments()
{
    auto documents = Vector<Document> {};

    documents.add({"Badge", badgeDocument, true});
    documents.add({"Features", featureDocument, true});
    documents.add({"Tiles - abutting edges", makeTilesDocument(16, 12), false});
    documents.add({"Stacked - 300 large shapes", makeStackedDocument(300), false});

    return documents;
}

// Reads the host's figures at paint time rather than being told them, for the
// same reason ComponentTree's does: a repaint asked for from inside the frame
// that produced the numbers is cleared on its way out, so a label told them
// would sit one frame behind for ever on a tree that only redraws when something
// moves.
struct StatsBar final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        if (host == nullptr || document == nullptr)
            return;

        // What the document asks the atlas for, against what the atlas holds.
        // The demand is in device pixels, so it is the mask area times the
        // square of the backing scale; the supply is the atlas squared.
        auto scale = host->backingScale();
        auto asked = document->getTotalMaskArea() * scale * scale;
        auto held = (float) host->getAtlasSize() * (float) host->getAtlasSize();

        auto millions = [](float texels)
        {
            auto tenths = (int) std::round(texels / 100000.f);
            return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
        };

        auto text =
            std::to_string(document->getShapeCount()) + " masks   "
            + std::to_string(document->getFontCount()) + " fonts   " + "asks "
            + millions(asked) + "M texels of a " + millions(held) + "M atlas   "
            + std::to_string((int) (host->getAtlasFillFraction() * 100.f))
            + "% reserved   " + std::to_string(document->getDroppedShapeCount())
            + " dropped   " + std::to_string(host->getLastClipChangeCount())
            + " breaks";

        g.setColour(UI::defaultTheme().dimText);
        g.drawText(text, getLocalBounds(), UI::Justification::Left);

        if (text != lastPainted)
        {
            lastPainted = text;
            LOG(text);
            Threads::callAsync([this] { repaint(); });
        }
    }

    UI::ComponentHost* host = nullptr;
    SVG::SVGComponent* document = nullptr;
    std::string lastPainted;
};

struct ComponentSide final : UI::Component
{
    ComponentSide()
    {
        title.setColour(UI::defaultTheme().text);
        stats.document = &document;

        addAndMakeVisible(title);
        addAndMakeVisible(document);
        addAndMakeVisible(stats);
    }

    void show(const Document& toShow)
    {
        title.setText("Component tier — " + toShow.name);

        auto parsed = SVG::parseXML(toShow.markup);

        if (parsed.has_value())
            document.setDocument(*parsed);

        repaint();
    }

    void paint(UI::Graphics& g) override { g.fillAll(Graphics::Color::white()); }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding);

        title.setBounds(area.removeFromTop(22.f));
        stats.setBounds(area.removeFromBottom(20.f));
        area.removeFromBottom(6.f);

        document.setBounds(area);
    }

    UI::Label title;
    SVG::SVGComponent document;
    StatsBar stats;
};

struct ComponentHostView final : UI::ComponentHost
{
    ComponentHostView()
    {
        setBackgroundColour(Graphics::Color::white());
        setFontPointSize(12.f);
        side.stats.host = this;
        setRootComponent(side);
    }

    ComponentSide side;
};

// The two tiers as siblings in one window: a native view holding native layers,
// beside a GPUView holding a component tree.
// Which document to open with, as argv[1], so a screenshot of any of them can be
// taken without anyone having to click through the others first.
int startingDocument()
{
    auto& args = Apps::getAppEnvironment().commandLineArgs;

    return args.size() > 1 ? Strings::parseIntOr(args[1], 0) : 0;
}

struct SplitView final : Graphics::View
{
    SplitView()
    {
        setHandlesMouseEvents(true);
        addSubview(componentHost);
        showDocument(startingDocument());
    }

    void showDocument(int index)
    {
        current = (index + documents.size()) % documents.size();

        auto& document = documents[current];

        if (nativeResult.root != nullptr)
            removeSubview(*nativeResult.root);

        if (document.nativeCanAfford)
            nativeResult = SVG::parse(document.markup);
        else
            nativeResult = {};

        if (nativeResult.root != nullptr)
        {
            addSubview(*nativeResult.root);
            nativeResult.root->stretchToFit();
        }

        componentHost.side.show(document);
        resized();
    }

    void mouseDown(const Graphics::MouseEvent&) override
    {
        showDocument(current + 1);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto half = area.w * 0.5f;

        if (nativeResult.root != nullptr)
        {
            nativeResult.root->setBounds({padding,
                                          padding + 22.f,
                                          half - padding * 2.f,
                                          area.h - padding * 2.f - 22.f});
            nativeResult.root->stretchToFit();
        }

        componentHost.setBounds({half, 0.f, area.w - half, area.h});
    }

    Vector<Document> documents {makeDocuments()};
    int current = 0;

    SVG::ParseResult nativeResult;
    ComponentHostView componentHost;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1180;
    options.height = 660;
    options.title = "eacp SVG — native layers | component tier (click to cycle)";
    options.minWidth = 640;
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

int main(int argc, char* argv[])
{
    return eacp::Apps::run<App>(argc, argv);
}
