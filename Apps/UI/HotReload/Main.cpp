#include "LayoutABI.h"

#include <eacp/UI/UI.h>
#include <eacp/Core/Plugins/HotReload.h>
#include <eacp/Core/Plugins/HotReloadLoop.h>

#include <optional>
#include <string>

// A window whose layout is one eacp-ui function in Layout.cpp beside this file.
// The app owns every widget; the function is handed them and the window's
// bounds and calls setBounds on each. It is always compiled into the app, and
// under EACP_HOT_RELOAD it is also built as a module, HotReloadLayout, bound at
// load time to this app's own eacp: edit a constant in Layout.cpp and save, a
// Plugins::HotReloadLoop rebuilds and reloads the module, and the fresh image
// is applied over the compiled-in copy.

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
        g.setColour(UI::defaultTheme().panel);
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

std::string describe(const UI::Rect& rect)
{
    return std::to_string(int(rect.x)) + "," + std::to_string(int(rect.y)) + " "
           + std::to_string(int(rect.w)) + "x" + std::to_string(int(rect.h));
}

struct DemoRoot final : UI::Component
{
    DemoRoot()
    {
        title.setFontSize(20.f);
        footer.setColour(UI::defaultTheme().dimText);
        status.setColour(UI::defaultTheme().dimText);
        notes.setPlaceholder("Notes");

        auto resetGain = [this] { gain.setValue(0.5f); };
        auto randomiseGain = [this]
        { gain.setValue(0.1f + 0.8f * gain.getValue()); };
        sidebar.reset.onClick = resetGain;
        sidebar.randomise.onClick = randomiseGain;

        addChildren({title, sidebar, gain, mute, notes, footer, status});

        if constexpr (Plugins::isHotReload())
            startReloadLoop();
    }

    void startReloadLoop()
    {
        auto& loop =
            reloader.emplace(Plugins::hotReloadTarget(), layoutFunctionName);

        auto showState = [this] { showStatus(); };
        auto useImage = [this](Plugins::ReloadableLibrary& library)
        { useLayoutFrom(library); };
        auto printFailure = [this](const auto&)
        { Console::print(reloader->describeState()); };

        loop.onStateChanged = showState;
        loop.onReloaded = useImage;
        loop.onBuildFailed = printFailure;
        loop.onLoadFailed = printFailure;
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().background);
    }

    void resized() override
    {
        placeStatus();
        applyLayout();
    }

    void useLayoutFrom(Plugins::ReloadableLibrary& library)
    {
        layout = library.findFunction<LayoutFunction>(layoutFunctionName);
        applyLayout();
        Console::print("generation " + std::to_string(library.getGeneration())
                       + ": gain at " + describe(gain.getBounds()));
    }

    void placeStatus()
    {
        auto area = getLocalBounds().inset(statusPadding);
        status.setBounds(area.removeFromBottom(statusHeight));
    }

    void applyLayout()
    {
        layout(targets, getLocalBounds());
        showStatus();
    }

    void showStatus()
    {
        status.setText(reloader ? reloader->describeState() : "static layout");
    }

    UI::Label title {"Hot-reloaded layout"};
    Sidebar sidebar;
    UI::Slider gain;
    UI::Checkbox mute {"Mute"};
    UI::TextEditor notes;
    UI::Label footer {"Edit Layout.cpp and save to move everything above"};
    UI::Label status;

    LayoutTargets targets {title, sidebar, gain, mute, notes, footer};
    LayoutFunction layout = &demoLayout;
    std::optional<Plugins::HotReloadLoop> reloader;
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
