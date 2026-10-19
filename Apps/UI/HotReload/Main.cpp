#include "LayoutABI.h"

#include <eacp/UI/UI.h>
#include <eacp/Core/Plugins/HotReload.h>

#include <optional>
#include <string>

// A window whose layout is one eacp-ui function in Layout.cpp beside this file.
// The app owns every widget; the function is handed them and the window's
// bounds and calls setBounds on each. It is always compiled into the app, and
// under EACP_HOT_RELOAD it is also built as a module, HotReloadLayout, bound
// at load time to this app's own eacp: edit a constant in Layout.cpp and save, the
// watcher sees it, cmake rebuilds the module, and the fresh image is loaded
// and applied over the compiled-in copy.

using namespace eacp;

namespace
{
constexpr auto statusHeight = 22.f;
constexpr auto statusPadding = 16.f;
constexpr auto logLimit = 4000;

std::string buildFailureSummary(const std::string& log)
{
    auto error = Strings::lineContaining(log, "error:");
    return error.empty() ? Strings::lastNonEmptyLine(log) : error;
}

Files::FileWatcherOptions makeWatcherOptions()
{
    auto options = Files::FileWatcherOptions {};
    options.pattern = "*.cpp;*.h";
    return options;
}

struct ReloadLoop
{
    ReloadLoop(const Plugins::HotReloadTarget& target,
               const Files::FileWatcher::ChangedFunc& onChanged)
        : library {FilePath {target.library},
                   FilePath::appCacheDirectory() / "HotReload"}
        , runner {Plugins::hotReloadBuildCommand(target)}
        , watcher {
              {FilePath {target.sourceDirectory}}, onChanged, makeWatcherOptions()}
    {
    }

    Plugins::ReloadableLibrary library;
    Processes::BuildRunner runner;
    Files::FileWatcher watcher;
};

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
        auto changed = [this](const Vector<FilePath>&) { sourcesChanged(); };
        auto& loop = reloader.emplace(Plugins::hotReloadTarget(), changed);

        auto appendOutput = [this](std::string_view chunk) { appendLog(chunk); };
        auto buildFinished = [this](const Processes::ProcessResult& result)
        { finishBuild(result); };
        loop.runner.onOutput = appendOutput;
        loop.runner.onFinished = buildFinished;
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

    void sourcesChanged()
    {
        status.setText("building…");
        reloader->runner.run();
    }

    void appendLog(std::string_view chunk)
    {
        log += chunk;

        if (log.size() > logLimit)
            log.erase(0, log.size() - logLimit);
    }

    void finishBuild(const Processes::ProcessResult& result)
    {
        auto summary = buildFailureSummary(log);
        log.clear();

        if (!result.exited || result.exitCode != 0)
        {
            status.setText("build failed: " + summary);
            Console::print("build failed: " + summary);
            return;
        }

        reloadLayout();
    }

    void reloadLayout()
    {
        auto& library = reloader->library;

        if (!library.reload())
        {
            reportLoadFailure(library.lastError());
            return;
        }

        auto loaded = library.findFunction<LayoutFunction>(layoutFunctionName);

        if (loaded == nullptr)
        {
            layout = &demoLayout;
            applyLayout();
            reportLoadFailure("no " + std::string {layoutFunctionName});
            return;
        }

        layout = loaded;
        applyLayout();
        Console::print("generation " + std::to_string(generation()) + ": gain at "
                       + describe(gain.getBounds()));
    }

    void reportLoadFailure(const std::string& reason)
    {
        status.setText("load failed: " + reason);
        Console::print("load failed: " + reason);
    }

    int generation() const
    {
        return reloader ? reloader->library.getGeneration() : 0;
    }

    void placeStatus()
    {
        auto area = getLocalBounds().inset(statusPadding);
        status.setBounds(area.removeFromBottom(statusHeight));
    }

    void applyLayout()
    {
        layout(targets, getLocalBounds());
        showLoaded();
    }

    void showLoaded()
    {
        auto text = std::string {"static layout"};

        if constexpr (Plugins::isHotReload())
            text = "generation " + std::to_string(generation()) + " · watching "
                   + std::string {Plugins::hotReloadTarget().sourceDirectory};

        status.setText(text);
    }

    UI::Label title {"Hot-reloaded layout"};
    Sidebar sidebar;
    UI::Slider gain;
    UI::Checkbox mute {"Mute"};
    UI::TextEditor notes;
    UI::Label footer {"Edit Layout.cpp and save to move everything above"};
    UI::Label status;

    LayoutTargets targets {title, sidebar, gain, mute, notes, footer};
    std::string log;
    LayoutFunction layout = &demoLayout;
    std::optional<ReloadLoop> reloader;
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
