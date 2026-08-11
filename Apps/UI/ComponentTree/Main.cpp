#include <eacp/UI/UI.h>

#include <string>

// A component tree in a single GPUView: 48 strips of five components each. The
// footer reports how many components the frame drew against how few batch breaks
// that cost, since every knob's vector path is one quad out of a shared atlas.

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

// Reads the host's figures at paint time rather than being told them: they are
// only complete once the walk that produced them is over, so the label paints
// the last frame's and asks for one more to catch up.
struct StatsBar final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        if (host == nullptr)
            return;

        auto text =
            std::to_string(host->getLastComponentCount()) + " components   "
            + std::to_string(host->getLastClipChangeCount()) + " batch breaks   "
            + std::to_string(host->getLastPaintedComponentCount()) + " painted   "
            + std::to_string(host->getLastSharedMaskCount()) + " masks shared";

        g.setColour(UI::defaultTheme().dimText);
        g.drawText(text, getLocalBounds(), UI::Justification::Right);

        // Posted to the loop rather than asked for here: a frame requested from
        // inside the native view's own draw cycle is coalesced into it.
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
