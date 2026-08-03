#include <eacp/UI/UI.h>

#include <string>

// A lightweight component tree in a single GPUView.
//
// What it is here to show is the cost model. Every strip below is five
// components -- a name, a rotary knob, a fader and two toggles -- inside a
// scrolling panel inside a root, and the footer reports how many components
// that came to and how many times the renderer had to break its batch to draw
// them. The second number stays in single figures while the first runs into the
// hundreds, because a component is not a native view and drawing one is queueing
// a quad.
//
// The knobs are the interesting ones. A knob's arc and pointer are a vector
// path, rasterized to exact per-pixel coverage by a compute kernel into a shared
// atlas -- so drawing one is also just a quad, and adding forty-eight of them to
// this tree does not add a single batch break. That is what the shared atlas
// buys: a coverage texture per path would have made each knob its own texture
// bind and its own draw.
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

        pan.setValue(0.15f
                     + 0.7f * static_cast<float>((indexToUse * 61) % 100) / 100.f);

        addChildren({name, pan, level, mute, solo});
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
        g.fillRoundedRect(getLocalBounds().inset(0.f, 2.f), 6.f);
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

        pan.setBounds(area.removeFromLeft(area.h));
        area.removeFromLeft(10.f);

        level.setBounds(area);
    }

    int index;
    UI::Label name;
    UI::Knob pan;
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

// Reads the host's figures at paint time rather than being told them.
//
// A component told the numbers would have to repaint to show a change, and a
// repaint asked for from inside the frame that produced them is lost -- so the
// label would sit one frame behind for ever, which on a tree that only redraws
// when something moves means it never updates at all.
struct StatsBar final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        if (host == nullptr)
            return;

        auto text = std::to_string(host->getLastComponentCount()) + " components   "
                    + std::to_string(host->getLastClipChangeCount())
                    + " batch breaks";

        g.setColour(UI::defaultTheme().dimText);
        g.drawText(text, getLocalBounds(), UI::Justification::Right);

        // The break count is only complete once the walk that produced it is
        // over, so this frame paints the last one's and needs one more to catch
        // up. Deferred rather than repainted on the spot because a repaint asked
        // for from inside the draw cycle is cleared on its way out; posting it
        // to the loop puts the request safely after the frame. Self-limiting --
        // once the figure stops changing, so does this.
        if (text != lastPainted)
        {
            lastPainted = text;
            LOG(text);
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

        selectAll.onClick = [this] { setAllToggles(true); };
        clearAll.onClick = [this] { setAllToggles(false); };

        list.setContent(strips);

        addChildren({title, selectAll, clearAll, list, stats});
    }

    void setAllToggles(bool shouldBeOn)
    {
        for (auto& strip: strips.strips)
        {
            strip->mute.setToggleState(shouldBeOn);
            strip->solo.setToggleState(shouldBeOn);
        }
    }

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
    StatsBar stats;
};

struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        root.stats.host = this;
        setRootComponent(root);
    }

    DemoRoot root;
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
