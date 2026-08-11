#include <eacp/UI/UI.h>

#include <cmath>
#include <string>

// 240 masked polygons, deliberately asking the shared coverage atlas for more
// than its 4096² holds. Shapes it refuses go missing rather than wrong: each
// tile keeps its frame and number, and the footer reports how many were dropped.

using namespace eacp;

namespace
{
constexpr auto tileCount = 240;
constexpr auto tileSize = 170.f;
constexpr auto padding = 12.f;

constexpr auto scrollPerSecond = 240.f;

// Sides run 3 to 11 so a mask sampled from the wrong atlas slot is visible as a
// polygon with the wrong number of sides.
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
        // A tile this size would default to triangles, so masks are asked for on
        // purpose - without them the atlas never fills.
        shape.setBacking(UI::PathShape::Backing::Mask);
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

        // Drawn whether or not the mask exists, so a dropped shape reads as an
        // empty frame rather than a gap in the layout.
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

// Reads the host's figures at paint time: a repaint asked for from inside the
// frame that produced them is cleared on the way out, so they are read during
// the next frame instead.
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

        addChildren({title, list, stats});
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

// Walks the list to the bottom once, so the tail - where the shapes the atlas
// had no room for are - is on screen without anybody touching the wheel.
struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        root.stats.host = this;
        setRootComponent(root);

        onPathsDropped = [](int count)
        { LOG("coverage atlas: ", count, " shapes with no room in it"); };

        // DisplayLink rather than a Threads::Timer: a Windows timer is a
        // WM_TIMER, which rounds its period up to the ~15.6ms system tick and so
        // delivers 60Hz as a stuttering 39Hz.
        setContinuous(true);
    }

    void update(Threads::FrameTime frame) override
    {
        auto step = scrollPerSecond * (float) frame.delta;

        // The first tick carries no delta, which is not the panel refusing to
        // move - the clamp test below would read it as the bottom.
        if (step <= 0.f)
            return;

        auto before = root.list.getScrollPosition();
        root.list.setScrollPosition(before + step);

        auto clampedAtBottom = root.list.getScrollPosition() == before;

        if (clampedAtBottom && !stopping)
        {
            stopping = true;

            // From a fresh stack frame: this destroys the link whose callback
            // this is running inside.
            Threads::callAsync([this] { setContinuous(false); });
        }
    }

    DemoRoot root;
    bool stopping = false;
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
