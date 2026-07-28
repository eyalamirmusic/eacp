#include <eacp/UI/UI.h>

#include <string>

// A lightweight component tree in a single GPUView.
//
// What it is here to show is the cost model. Every strip below is four
// components -- a name, a fader and two toggles -- inside a scrolling panel
// inside a root, and the footer reports how many components that came to and
// how many times the renderer had to break its batch to draw them. The second
// number stays in single figures while the first runs into the hundreds,
// because a component is not a native view and drawing one is queueing a quad.
//
// Scroll the list to see clipping: rows are cut at the panel's edge without the
// panel doing anything about it, since paint() is handed a Graphics already
// clipped to the component's own bounds.

using namespace eacp;

namespace
{
constexpr auto channelCount = 48;
constexpr auto stripHeight = 44.f;
constexpr auto padding = 12.f;

struct ChannelStrip final : UI::Component
{
    explicit ChannelStrip(int indexToUse)
        : index(indexToUse)
        , name("Channel " + std::to_string(indexToUse + 1))
    {
        name.setColour(UI::defaultTheme().dimText);

        level.setValue(0.35f
                       + 0.6f * static_cast<float>((indexToUse * 37) % 100) / 100.f);
        level.onValueChange = [this](float value) { levelChanged(value); };

        mute.setToggleable(true);
        mute.setAccentColour({0.92f, 0.55f, 0.25f, 1.f});

        solo.setToggleable(true);
        solo.setAccentColour({0.45f, 0.80f, 0.45f, 1.f});

        addAndMakeVisible(name);
        addAndMakeVisible(level);
        addAndMakeVisible(mute);
        addAndMakeVisible(solo);
    }

    void levelChanged(float value)
    {
        auto decibels = static_cast<int>(value * 100.f);
        name.setText("Channel " + std::to_string(index + 1) + "  ("
                     + std::to_string(decibels) + ")");
    }

    void paint(UI::Graphics& g) override
    {
        g.setColour(UI::defaultTheme().panel);
        g.fillRect(getLocalBounds().inset(0.f, 2.f));
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(8.f, 6.f);

        solo.setBounds(area.removeFromRight(46.f));
        area.removeFromRight(4.f);
        mute.setBounds(area.removeFromRight(46.f));
        area.removeFromRight(10.f);

        name.setBounds(area.removeFromLeft(150.f));
        area.removeFromLeft(10.f);

        level.setBounds(area);
    }

    int index;
    UI::Label name;
    UI::Slider level;
    UI::Button mute {"Mute"};
    UI::Button solo {"Solo"};
};

// The scrolled content: every strip, stacked. Its height is the sum, which is
// what makes it taller than the panel and therefore scrollable.
struct StripList final : UI::Component
{
    StripList()
    {
        strips.reserve(channelCount);

        for (auto index = 0; index < channelCount; ++index)
        {
            auto& strip = strips.add(makeOwned<ChannelStrip>(index));
            addAndMakeVisible(*strip);
        }

        setBounds({0.f, 0.f, 0.f, channelCount * stripHeight});
    }

    void resized() override
    {
        auto y = 0.f;

        for (auto& strip: strips)
        {
            strip->setBounds({0.f, y, getWidth(), stripHeight});
            y += stripHeight;
        }
    }

    OwnedVector<ChannelStrip> strips;
};

struct DemoRoot final : UI::Component
{
    DemoRoot()
    {
        title.setColour(UI::defaultTheme().text);

        selectAll.onClick = [this] { setAllToggles(true); };
        clearAll.onClick = [this] { setAllToggles(false); };

        stats.setColour(UI::defaultTheme().dimText);
        stats.setJustification(UI::Justification::Right);

        list.setContent(strips);

        addAndMakeVisible(title);
        addAndMakeVisible(selectAll);
        addAndMakeVisible(clearAll);
        addAndMakeVisible(list);
        addAndMakeVisible(stats);
    }

    void setAllToggles(bool shouldBeOn)
    {
        for (auto& strip: strips.strips)
        {
            strip->mute.setToggleState(shouldBeOn);
            strip->solo.setToggleState(shouldBeOn);
        }
    }

    void setStatsText(std::string text) { stats.setText(std::move(text)); }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().background);
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding);

        auto header = area.removeFromTop(32.f);
        clearAll.setBounds(header.removeFromRight(90.f));
        header.removeFromRight(6.f);
        selectAll.setBounds(header.removeFromRight(90.f));
        title.setBounds(header);

        area.removeFromTop(padding);

        auto footer = area.removeFromBottom(22.f);
        stats.setBounds(footer);

        area.removeFromBottom(6.f);
        list.setBounds(area);
    }

    UI::Label title {"eacp UI — component tree in one GPUView"};
    UI::Button selectAll {"All on"};
    UI::Button clearAll {"All off"};
    UI::ScrollPanel list;
    StripList strips;
    UI::Label stats;
};

struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        setRootComponent(root);
    }

    void render(GPU::Frame& frame) override
    {
        ComponentHost::render(frame);

        // Read back after the walk, so the numbers are the ones this frame
        // actually produced rather than the previous frame's.
        auto text = std::to_string(getLastComponentCount()) + " components   "
                    + std::to_string(getLastClipChangeCount()) + " batch breaks";

        if (text != lastStatsText)
        {
            lastStatsText = text;
            root.setStatsText(text);
        }
    }

    DemoRoot root;
    std::string lastStatsText;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 900;
    options.height = 640;
    options.title = "eacp UI — component tree";
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
