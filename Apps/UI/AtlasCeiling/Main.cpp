#include <eacp/UI/UI.h>

#include <cmath>
#include <string>

// What an interface looks like when it asks the coverage atlas for more than the
// atlas has.
//
// Every vector shape in a component tree is rasterized into one shared texture,
// which is what makes a path cost a quad instead of a texture bind. That texture
// grows to 4096 square and then stops, and a tree whose masks do not all fit in
// it at once loses the ones that arrive last. This is that tree: 240 tiles, each
// with a polygon in it, which at two device pixels to the point comes to some 19
// million texels against the atlas's 16.8 million.
//
// Two things are worth watching, and the footer reports both.
//
// The first is that the shapes that did not fit are *missing* rather than wrong.
// Every tile keeps its frame and its number, so an empty frame is a mask that
// was refused - and refusing is a decision, taken because the alternative is
// worse. An atlas that made room on the pass whose layout is being drawn would
// relocate the shapes already placed, and each of those would then sample texels
// that now belong to somebody else: a tile drawing another tile's polygon. That
// is why the polygons have a different number of sides each. A seven-sided tile
// showing a triangle is what the wrong answer looks like, and it is not
// something you would notice in a screenshot of circles.
//
// The second is that reaching this at all takes a tree with more in it than the
// window shows. A mask is the size of the shape on screen, so everything visible
// at once cannot come to more than the window's own area -- this window is 3.2
// million device pixels against the atlas's 16.8 million, and a window of solid
// vector art would still be a fifth of it. It is scrolled-away content, hidden
// tabs and stacked shapes that reach the ceiling, which is why this demo is a
// list forty rows long rather than a full window of vector art.

using namespace eacp;

namespace
{
constexpr auto tileCount = 240;
constexpr auto tileSize = 170.f;
constexpr auto padding = 12.f;

// A tile's shape, and the whole point of it being a different shape per tile: a
// mask drawn through the wrong slot is a polygon with the wrong number of sides,
// which is visible. Sides run 3 to 11 and the rotation varies too, so no two
// neighbours look alike.
GPUWidgets::Path polygon(const Graphics::Rect& area, int index)
{
    auto sides = 3 + index % 9;
    auto turn = 6.28318531f / (float) sides;
    auto phase = 0.37f * (float) index;

    auto radius = 0.5f * std::min(area.w, area.h);
    auto centreX = area.x + 0.5f * area.w;
    auto centreY = area.y + 0.5f * area.h;

    auto path = GPUWidgets::Path {};

    for (auto corner = 0; corner < sides; ++corner)
    {
        auto angle = phase + turn * (float) corner;
        auto point = Graphics::Point {centreX + radius * std::cos(angle),
                                      centreY + radius * std::sin(angle)};

        if (corner == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }

    path.close();
    return path;
}

struct Tile final : UI::Component
{
    explicit Tile(int indexToUse)
        : index(indexToUse)
    {
    }

    void resized() override
    {
        shape.setPath(polygon(getLocalBounds().inset(14.f), index));
    }

    void paint(UI::Graphics& g) override
    {
        const auto& theme = UI::defaultTheme();

        g.setColour(theme.panel);
        g.fillRoundedRect(getLocalBounds().inset(3.f), 6.f);

        // Drawn whether or not the mask exists, so a shape the atlas had no room
        // for reads as an empty frame rather than as a gap in the layout.
        g.setColour(theme.outline);
        g.drawRect(getLocalBounds().inset(3.f));

        g.setColour(theme.accent);
        g.fillPath(shape);

        g.setColour(theme.dimText);
        g.drawText(std::to_string(index),
                   getLocalBounds().inset(8.f, 4.f),
                   UI::Justification::Left);
    }

    int index;
    UI::PathShape shape {*this};
};

struct TileGrid final : UI::Component
{
    TileGrid()
    {
        tiles.reserve(tileCount);

        for (auto index = 0; index < tileCount; ++index)
            addAndMakeVisible(*tiles.add(makeOwned<Tile>(index)));
    }

    void resized() override
    {
        auto columns = std::max(1, (int) (getWidth() / tileSize));

        for (auto index = 0; index < tiles.size(); ++index)
        {
            auto column = index % columns;
            auto row = index / columns;

            tiles[index]->setBounds({(float) column * tileSize,
                                     (float) row * tileSize,
                                     tileSize,
                                     tileSize});
        }

        auto rows = (tiles.size() + columns - 1) / columns;
        setBounds({0.f, 0.f, getWidth(), (float) rows * tileSize});
    }

    OwnedVector<Tile> tiles;
};

// Reads the host's figures at paint time, for the reason ComponentTree's does:
// a repaint asked for from inside the frame that produced them is cleared on the
// way out, so anything derived from a frame has to be read during the next one.
struct StatsBar final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        if (host == nullptr)
            return;

        auto dropped = host->getLastDroppedPathCount();
        auto percent = (int) (100.f * host->getAtlasFillFraction() + 0.5f);

        auto text = std::to_string(tileCount) + " shapes   atlas "
                    + std::to_string(host->getAtlasSize()) + "²  "
                    + std::to_string(percent) + "% full   " + std::to_string(dropped)
                    + " dropped";

        g.setColour(dropped > 0 ? UI::defaultTheme().text
                                : UI::defaultTheme().dimText);
        g.drawText(text, getLocalBounds(), UI::Justification::Right);

        if (text != lastPainted)
        {
            lastPainted = text;
            Threads::callAsync([this] { repaint(); });
        }
    }

    UI::ComponentHost* host = nullptr;
    std::string lastPainted;
};

struct DemoRoot final : UI::Component
{
    DemoRoot()
    {
        title.setColour(UI::defaultTheme().text);

        list.setContent(grid);

        addAndMakeVisible(title);
        addAndMakeVisible(list);
        addAndMakeVisible(stats);
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().background);
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding);

        title.setBounds(area.removeFromTop(28.f));
        area.removeFromTop(padding);

        stats.setBounds(area.removeFromBottom(22.f));
        area.removeFromBottom(6.f);

        list.setBounds(area);
    }

    UI::Label title {"eacp UI — more vector shapes than the coverage atlas holds"};
    UI::ScrollPanel list;
    TileGrid grid;
    StatsBar stats;
};

// Walks the list to the bottom once, so the tail - which is where the shapes
// the atlas had no room for are - is on screen without anybody touching the
// wheel. It stops there, and the wheel still works.
struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        root.stats.host = this;
        setRootComponent(root);

        onPathsDropped = [](int count)
        { LOG("coverage atlas: ", count, " shapes with no room in it"); };
    }

    // Stops of its own accord: the panel clamps at the bottom and ignores a
    // position it is already at, so the frames stop coming with it.
    void step() { root.list.setScrollPosition(root.list.getScrollPosition() + 6.f); }

    DemoRoot root;

    Threads::Timer scroller {[this] { step(); }, 60};
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1100;
    options.height = 720;
    options.title = "eacp UI — the coverage atlas ceiling";
    options.minWidth = 480;
    options.minHeight = 320;

    return options;
}

struct App
{
    App() { window.setContentView(host); }

    DemoHost host;
    Graphics::Window window {makeOptions()};
};
} // namespace

int main()
{
    return eacp::Apps::run<App>();
}
