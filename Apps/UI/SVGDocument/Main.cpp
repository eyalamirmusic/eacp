#include <eacp/SVG/SVG.h>
#include <eacp/UI/UI.h>

#include <cmath>
#include <string>

// The same SVG document drawn twice: on the left one Graphics::ShapeLayer per
// shape, on the right one UI::PathShape per shape, drawn as quads out of a
// shared atlas. Click to cycle documents; the footer reports the atlas figures.

using namespace eacp;

namespace
{
constexpr auto padding = 12.f;

const auto badgeDocument = std::string {
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
  <rect x="10" y="10" width="180" height="180" rx="20" fill="#4A90D9" stroke="#2C5F8A" stroke-width="3"/>
  <circle cx="100" cy="80" r="30" fill="#F5A623"/>
  <text x="100" y="160" text-anchor="middle" font-family="Helvetica" font-size="18" fill="white">eacp SVG</text>
</svg>)SVG"};

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

// The two halves are supposed to disagree here: the native side draws the arcs
// and nothing else, since it resolves no <use>, reads no style="" and has no
// dash operation behind stroke-dasharray.
const auto documentFeatureDocument = std::string {
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="320" height="240" viewBox="0 0 320 240">
  <defs>
    <symbol id="star" viewBox="0 0 20 20">
      <polygon points="10,1 12.6,7.2 19.5,7.6 14.2,12 15.9,18.6 10,15 4.1,18.6 5.8,12 0.5,7.6 7.4,7.2"
               fill="#F5A623" stroke="#8C6212" stroke-width="0.6" stroke-linejoin="round"/>
    </symbol>
    <!-- The head is two arcs, which is how a full circle is written in path
         data. Its stem is wound the same way round on purpose: two contours of
         one path that disagree cancel where they overlap under the non-zero
         rule, which is a hairline of background across the join and not a
         renderer bug. -->
    <path id="pin" d="M -9 0 A 9 9 0 1 1 9 0 A 9 9 0 1 1 -9 0 Z M 5 6 L 0 21 L -5 6 Z"/>
  </defs>

  <rect x="0" y="0" width="320" height="240" fill="#FBF8F3"/>

  <g style="fill:#3B7A57;stroke:#204A34;stroke-width:2;stroke-linejoin:round" fill="#D0021B">
    <path d="M 26 62 A 40 40 0 0 1 106 62 L 66 62 Z"/>
    <path d="M 130 20 h 44 a 12 12 0 0 1 12 12 v 24 a 12 12 0 0 1 -12 12 h -44 a 12 12 0 0 1 -12 -12 v -24 a 12 12 0 0 1 12 -12 z"/>
  </g>

  <use href="#star" x="216" y="16" width="34" height="34"/>
  <use href="#star" x="254" y="12" width="48" height="48"/>
  <use href="#star" x="222" y="58" width="22" height="22"/>

  <circle cx="66" cy="158" r="38" fill="none" stroke="#7A4FA3" stroke-width="4" stroke-dasharray="10 6"/>
  <circle cx="66" cy="158" r="27" fill="none" stroke="#D96A4A" stroke-width="4" stroke-dasharray="10 6" stroke-dashoffset="8"/>

  <path d="M 122 198 C 162 148, 214 248, 254 198" fill="none" stroke="#2C5F8A"
        stroke-width="3" stroke-dasharray="1 7" stroke-linecap="round"/>

  <g fill="#4A90D9" stroke="#2C5F8A" stroke-width="1.5">
    <use href="#pin" x="146" y="116"/>
    <use href="#pin" x="196" y="134"/>
    <use href="#pin" x="246" y="116"/>
  </g>

  <text x="160" y="230" text-anchor="middle" font-family="Helvetica" font-size="11"
        fill="#D0021B" style="fill:#8A7A6A">arcs · use · symbol · dashes · style</text>
</svg>)SVG"};

// Gradients, which the native half cannot draw at all.
const auto gradientDocument = std::string {
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="360" height="300" viewBox="0 0 360 300">
  <defs>
    <linearGradient id="sunset">
      <stop offset="0" stop-color="#F5A623"/>
      <stop offset="0.45" stop-color="#D0021B"/>
      <stop offset="1" stop-color="#4A2E7A"/>
    </linearGradient>

    <!-- The same colours, another axis: only the geometry is restated, and the
         stops come through href. -->
    <linearGradient id="sunsetDiagonal" href="#sunset" x1="0" y1="0" x2="1" y2="1"/>

    <linearGradient id="band" x1="0" y1="0" x2="0.25" y2="0">
      <stop offset="0" stop-color="#2C5F8A"/>
      <stop offset="1" stop-color="#FBF8F3"/>
    </linearGradient>
    <linearGradient id="bandReflect" href="#band" spreadMethod="reflect"/>
    <linearGradient id="bandRepeat" href="#band" spreadMethod="repeat"/>

    <radialGradient id="globe" cx="0.35" cy="0.3" r="0.75">
      <stop offset="0" stop-color="#FFFFFF"/>
      <stop offset="0.5" stop-color="#4A90D9"/>
      <stop offset="1" stop-color="#173F63"/>
    </radialGradient>

    <!-- userSpaceOnUse, so this one is in the document's own coordinates and
         paints the same stripe across whatever it is given, plus a skew no pair
         of endpoints could describe. -->
    <linearGradient id="skewed" gradientUnits="userSpaceOnUse"
                    x1="20" y1="0" x2="90" y2="0"
                    gradientTransform="skewX(35)" spreadMethod="reflect">
      <stop offset="0" stop-color="#3B7A57"/>
      <stop offset="1" stop-color="#F5A623"/>
    </linearGradient>
  </defs>

  <rect x="0" y="0" width="360" height="300" fill="#FBF8F3"/>

  <rect x="20" y="16" width="320" height="44" rx="10" fill="url(#sunsetDiagonal)"/>
  <rect x="20" y="70" width="80" height="80" fill="url(#sunsetDiagonal)"/>
  <circle cx="170" cy="110" r="40" fill="url(#globe)"/>
  <rect x="228" y="70" width="112" height="80" rx="8" fill="url(#skewed)"/>

  <rect x="20" y="166" width="100" height="34" fill="url(#band)"/>
  <rect x="130" y="166" width="100" height="34" fill="url(#bandReflect)"/>
  <rect x="240" y="166" width="100" height="34" fill="url(#bandRepeat)"/>

  <!-- A gradient on a stroke, and one inherited from a group by a child that
       says nothing about its own fill. -->
  <g fill="url(#sunset)">
    <path d="M 20 216 h 90 v 46 h -90 z"/>
    <circle cx="170" cy="239" r="23"/>
  </g>
  <path d="M 228 262 C 258 206, 288 292, 340 226" fill="none"
        stroke="url(#globe)" stroke-width="9" stroke-linecap="round"/>

  <text x="180" y="288" text-anchor="middle" font-family="Helvetica" font-size="11"
        fill="#7A6A5A">linear · radial · spread · units · href</text>
</svg>)SVG"};

// A rectangular clip is a scissor rect and costs the atlas nothing; any other
// shape is one mask over everything under it.
const auto clipDocument = std::string {
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="360" height="300" viewBox="0 0 360 300">
  <defs>
    <linearGradient id="clipFade" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#F5A623"/>
      <stop offset="1" stop-color="#D0021B"/>
    </linearGradient>

    <clipPath id="pane">
      <rect x="20" y="16" width="150" height="80"/>
    </clipPath>

    <!-- Two shapes, so the region is their union. -->
    <clipPath id="pair">
      <circle cx="228" cy="56" r="36"/>
      <circle cx="292" cy="56" r="36"/>
    </clipPath>

    <!-- In fractions of whatever it clips, so one definition is a different
         region for each of the two shapes below. -->
    <clipPath id="leftPart" clipPathUnits="objectBoundingBox">
      <rect x="0" y="0" width="0.55" height="1"/>
    </clipPath>

    <clipPath id="frame" clip-rule="evenodd">
      <path d="M 200 116 h 140 v 80 h -140 z M 224 136 h 92 v 40 h -92 z"/>
    </clipPath>

    <clipPath id="band">
      <rect x="20" y="216" width="150" height="46"/>
    </clipPath>
    <clipPath id="lens">
      <circle cx="96" cy="252" r="34"/>
    </clipPath>

    <clipPath id="caption">
      <rect x="200" y="230" width="96" height="36"/>
    </clipPath>
  </defs>

  <rect x="0" y="0" width="360" height="300" fill="#FBF8F3"/>

  <!-- Behind rather than around, and filled rather than stroked: an outline is
       one shape whose mask is its whole bounding box however little of that box
       is inked, and a fill this size is large enough to be drawn as triangles
       and cost the atlas nothing. -->
  <rect x="20" y="16" width="150" height="80" fill="#EDE6DA"/>

  <g clip-path="url(#pane)" fill="#4A90D9">
    <circle cx="40" cy="40" r="40"/>
    <circle cx="95" cy="72" r="40" fill="#3B7A57"/>
    <circle cx="150" cy="40" r="40" fill="#D96A4A"/>
  </g>

  <g clip-path="url(#pair)">
    <rect x="180" y="10" width="172" height="92" fill="url(#clipFade)"/>
    <rect x="180" y="36" width="172" height="10" fill="#FBF8F3" opacity="0.75"/>
    <rect x="180" y="66" width="172" height="10" fill="#FBF8F3" opacity="0.75"/>
  </g>

  <rect x="20" y="116" width="70" height="80" fill="url(#clipFade)" clip-path="url(#leftPart)"/>
  <circle cx="140" cy="156" r="40" fill="#2C5F8A" clip-path="url(#leftPart)"/>

  <rect x="200" y="116" width="140" height="80" fill="url(#clipFade)" clip-path="url(#frame)"/>

  <g clip-path="url(#band)">
    <rect x="20" y="216" width="150" height="72" fill="#F5A623"/>
    <circle cx="96" cy="252" r="46" fill="#204A34" clip-path="url(#lens)"/>
  </g>

  <text x="200" y="258" font-family="Helvetica" font-size="24" fill="#7A4FA3"
        clip-path="url(#caption)">clipped text</text>

  <text x="180" y="292" text-anchor="middle" font-family="Helvetica" font-size="11"
        fill="#7A6A5A">rect · union · units · clip-rule · nested</text>
</svg>)SVG"};

const auto opacityDocument = std::string {
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="360" height="300" viewBox="0 0 360 300">
  <defs>
    <linearGradient id="fade" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#F5A623"/>
      <stop offset="1" stop-color="#D0021B"/>
    </linearGradient>
  </defs>

  <rect x="0" y="0" width="360" height="300" fill="#FBF8F3"/>

  <!-- Each circle faded: the overlaps are darker, because two half-transparent
       discs are stacked there. -->
  <g>
    <circle cx="60" cy="70" r="38" fill="#2C5F8A" opacity="0.5"/>
    <circle cx="96" cy="70" r="38" fill="#2C5F8A" opacity="0.5"/>
    <circle cx="78" cy="104" r="38" fill="#2C5F8A" opacity="0.5"/>
  </g>

  <!-- The group faded: one texture, one fade, and the overlaps are the shape's
       own colour. -->
  <g opacity="0.5">
    <circle cx="244" cy="70" r="38" fill="#2C5F8A"/>
    <circle cx="280" cy="70" r="38" fill="#2C5F8A"/>
    <circle cx="262" cy="104" r="38" fill="#2C5F8A"/>
  </g>

  <text x="78" y="164" text-anchor="middle" font-family="Helvetica" font-size="11" fill="#7A6A5A">per element</text>
  <text x="262" y="164" text-anchor="middle" font-family="Helvetica" font-size="11" fill="#7A6A5A">per group</text>

  <!-- A group inside a group: the inner texture is rendered first and then
       drawn into the outer one, so the two fades multiply. -->
  <g opacity="0.7">
    <rect x="20" y="188" width="150" height="88" fill="#3B7A57"/>
    <g opacity="0.6">
      <circle cx="70" cy="232" r="34" fill="#F5A623"/>
      <circle cx="110" cy="232" r="34" fill="#F5A623"/>
    </g>
  </g>

  <!-- Text and a gradient inside one, both of which go through renderers of
       their own and both of which have to land in the texture. -->
  <g opacity="0.55">
    <rect x="200" y="188" width="140" height="88" rx="12" fill="url(#fade)"/>
    <circle cx="240" cy="232" r="30" fill="#204A34"/>
    <circle cx="290" cy="232" r="30" fill="#204A34"/>
    <text x="270" y="270" text-anchor="middle" font-family="Helvetica" font-size="13" fill="#FBF8F3">in the layer</text>
  </g>
</svg>)SVG"};

// A 320x120 document in a tall pane: letterboxed under preserveAspectRatio's
// default, stretched by the native side.
const auto aspectDocument = std::string {
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="320" height="120" viewBox="0 0 320 120">
  <rect x="0" y="0" width="320" height="120" fill="#EFE7DA"/>
  <circle cx="60" cy="60" r="44" fill="#4A90D9"/>
  <circle cx="160" cy="60" r="44" fill="#3B7A57"/>
  <circle cx="260" cy="60" r="44" fill="#D96A4A"/>
  <text x="160" y="66" text-anchor="middle" font-family="Helvetica" font-size="20" fill="#FBF8F3">round, not oval</text>
</svg>)SVG"};

// Triangles sharing exact vertices: every interior edge is antialiased twice by
// two shapes that know nothing of each other, which is where seams show.
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

// Large shapes stacked rather than tiled: each mask is about a ninth of the
// document's area, so the sum runs past the window's own area many times over.
std::string makeStackedDocument(int shapeCount)
{
    auto size = 1000;

    constexpr auto coprimeStepX = 137;
    constexpr auto coprimeStepY = 211;

    auto markup = std::string {R"(<svg xmlns="http://www.w3.org/2000/svg" width=")"}
                  + std::to_string(size) + R"(" height=")" + std::to_string(size)
                  + R"(" viewBox="0 0 )" + std::to_string(size) + " "
                  + std::to_string(size) + R"(">)";

    markup += R"(<rect x="0" y="0" width=")" + std::to_string(size) + R"(" height=")"
              + std::to_string(size) + R"(" fill="#F7F4EF"/>)";

    for (auto index = 0; index < shapeCount; ++index)
    {
        auto centreX = 120 + (index * coprimeStepX) % (size - 240);
        auto centreY = 120 + (index * coprimeStepY) % (size - 240);
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

    // False where the native side cannot pay for it: a shape layer is one
    // compositor surface the size of the view, so a 300-shape document asks the
    // window server for gigabytes.
    bool nativeCanAfford = true;
};

Vector<Document> makeDocuments()
{
    auto documents = Vector<Document> {};

    documents.add({"Badge", badgeDocument, true});
    documents.add({"Features", featureDocument, true});
    documents.add({"Document features - arcs, use, dashes, style",
                   documentFeatureDocument,
                   true});
    documents.add(
        {"Gradients - linear, radial, spread, units", gradientDocument, true});
    documents.add({"Clip paths - rect, union, units, nesting", clipDocument, true});
    documents.add(
        {"Group opacity - per element against per group", opacityDocument, true});
    documents.add({"Aspect ratio - fitted against stretched", aspectDocument, true});
    documents.add({"Tiles - abutting edges", makeTilesDocument(16, 12), false});
    documents.add({"Stacked - 300 large shapes", makeStackedDocument(300), false});

    return documents;
}

// Reads the host's figures at paint time: a repaint asked for from inside the
// frame that produced them is cleared on the way out, so they are read during
// the next frame instead.
struct StatsBar final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        if (host == nullptr || document == nullptr)
            return;

        // Demand is in device pixels: mask area times the square of the scale.
        auto scale = host->backingScale();
        auto asked = document->getAtlasMaskArea() * scale * scale;
        auto unmeshed = document->getTotalMaskArea() * scale * scale;
        auto held = (float) host->getAtlasSize() * (float) host->getAtlasSize();

        auto millions = [](float texels)
        {
            auto tenths = (int) std::round(texels / 100000.f);
            return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
        };

        auto text =
            std::to_string(document->getShapeCount()) + " shapes   "
            + std::to_string(document->getMeshedShapeCount()) + " meshed   "
            + std::to_string(document->getClipCount()) + " clips ("
            + std::to_string(document->getClipMaskCount()) + " masked)   "
            + std::to_string(document->getOpacityGroupCount()) + " groups ("
            + std::to_string(host->getLastRenderedLayerCount()) + " rendered)   "
            + std::to_string(document->getFontCount()) + " fonts   " + "asks "
            + millions(asked) + "M texels of a " + millions(held) + "M atlas ("
            + millions(unmeshed) + "M unmeshed)   "
            + std::to_string((int) (host->getAtlasFillFraction() * 100.f))
            + "% reserved   " + std::to_string(host->getLastDroppedPathCount())
            + " dropped   " + std::to_string(host->getLastClipChangeCount())
            + " breaks   " + std::to_string(host->getLastRendererSwitchCount())
            + " switches";

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

        addChildren({title, document, stats});
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

// Which document to open with, as argv[1], so any of them can be screenshotted
// without clicking through the others first.
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
