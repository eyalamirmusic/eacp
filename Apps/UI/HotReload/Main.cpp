#include "LayoutABI.h"

#include <eacp/Core/Plugins/Reloadable.h>
#include <eacp/UI/UI.h>

// A window whose layout is one eacp-ui function in Layout.cpp beside this file.
// The app owns every widget; the function is handed them and the window's
// bounds and calls setBounds on each. Under EACP_HOT_RELOAD, edit a constant in
// Layout.cpp and save: Plugins::Reloadable rebuilds the module and swaps the
// function in. See Docs/HotReload.md.

using namespace eacp;

namespace
{
constexpr auto statusHeight = 22.f;
constexpr auto statusPadding = 16.f;

struct Sidebar final : UI::Component
{
    Sidebar() { addChildren({heading, reset, randomise}); }

    void paint(UI::Graphics& g) override
    {
        g.setColour(UI::defaultTheme().get(UI::ThemeKeys::panel));
        g.fillRoundedRect(getLocalBounds(), 8.f);
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(10.f);
        heading.setBounds(area.removeFromTop(24.f));
        area.removeFromTop(8.f);
        reset.setBounds(area.removeFromTop(30.f));
        area.removeFromTop(6.f);
        randomise.setBounds(area.removeFromTop(30.f));
    }

    UI::Label heading {"Presets"};
    UI::Button reset {"Reset"};
    UI::Button randomise {"Randomise"};
};

struct DemoRoot final : UI::Component
{
    DemoRoot()
    {
        title.setFontSize(20.f);
        footer.setThemeColour(UI::ThemeKeys::dimText);
        status.setThemeColour(UI::ThemeKeys::dimText);
        notes.setPlaceholder("Notes");

        auto resetGain = [this] { gain.setValue(0.5f); };
        auto randomiseGain = [this]
        { gain.setValue(0.1f + 0.8f * gain.getValue()); };
        sidebar.reset.onClick = resetGain;
        sidebar.randomise.onClick = randomiseGain;

        addChildren({title, sidebar, gain, mute, notes, footer, status});
        showStatus();
    }

    Plugins::ReloadableOptions reloadOptions()
    {
        auto options = Plugins::ReloadableOptions {};
        auto relayout = [this] { applyLayout(); };
        auto showState = [this] { showStatus(); };
        options.onReloaded = relayout;
        options.onStateChanged = showState;
        return options;
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().get(UI::ThemeKeys::background));
    }

    void resized() override
    {
        placeStatus();
        applyLayout();
    }

    void placeStatus()
    {
        auto area = getLocalBounds().inset(statusPadding);
        status.setBounds(area.removeFromBottom(statusHeight));
    }

    void applyLayout()
    {
        reloadable.call<LayoutFunction>(targets, getLocalBounds());
    }

    void showStatus() { status.setText("layout: " + reloadable.describeState()); }

    UI::Label title {"Hot-reloaded layout"};
    Sidebar sidebar;
    UI::Slider gain;
    UI::Checkbox mute {"Mute"};
    UI::TextEditor notes;
    UI::Label footer {"Edit Layout.cpp and save to move everything above"};
    UI::Label status;

    LayoutTargets targets {title, sidebar, gain, mute, notes, footer};
    Plugins::Reloadable reloadable {reloadOptions()};
};

struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        setRootComponent(root);
    }

    DemoRoot root;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 860;
    options.height = 560;
    options.title = "eacp UI — hot-reloaded layout";
    options.minWidth = 480;
    options.minHeight = 320;
    return options;
}
} // namespace

int main()
{
    return Graphics::runWindowedApp<DemoHost>(makeOptions());
}
