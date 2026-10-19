#include "LayoutABI.h"

#include <eacp/UI/UI.h>

#include <string>

// A window whose layout lives in a separate module, HotReloadLayout, built from
// Layout.cpp beside this file. The app owns every widget; the module is one C
// function that is handed the window's size and answers with a rectangle per
// named widget. Edit a constant in Layout.cpp and save: the watcher sees it,
// cmake rebuilds the module, and the fresh image is loaded and applied.

using namespace eacp;

namespace
{
constexpr auto layoutCapacity = 64;
constexpr auto statusHeight = 22.f;
constexpr auto statusPadding = 16.f;
constexpr auto logLimit = 4000;

std::string lastNonEmptyLine(const std::string& text)
{
    auto end = text.find_last_not_of("\r\n \t");

    if (end == std::string::npos)
        return {};

    auto start = text.find_last_of("\r\n", end);
    auto first = start == std::string::npos ? 0 : start + 1;
    return text.substr(first, end + 1 - first);
}

std::string lineContaining(const std::string& text, std::string_view needle)
{
    auto found = text.find(needle);

    if (found == std::string::npos)
        return {};

    auto start = text.find_last_of("\r\n", found);
    auto first = start == std::string::npos ? 0 : start + 1;
    auto last = text.find_first_of("\r\n", found);
    return text.substr(first, last == std::string::npos ? last : last - first);
}

std::string buildFailureSummary(const std::string& log)
{
    auto error = lineContaining(log, "error:");
    return error.empty() ? lastNonEmptyLine(log) : error;
}

Processes::ProcessOptions makeBuildCommand()
{
    auto options = Processes::ProcessOptions {};
    options.executable = EACP_HOT_RELOAD_CMAKE;
    options.arguments = {"--build",
                         EACP_HOT_RELOAD_BUILD_DIR,
                         "--config",
                         EACP_HOT_RELOAD_CONFIG,
                         "--target",
                         EACP_HOT_RELOAD_TARGET};
    return options;
}

Files::FileWatcherOptions makeWatcherOptions()
{
    auto options = Files::FileWatcherOptions {};
    options.pattern = "*.cpp;*.h";
    return options;
}

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

struct NamedComponent
{
    std::string key;
    UI::Component* component = nullptr;
};

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

        named.add({"title", &title});
        named.add({"sidebar", &sidebar});
        named.add({"gain", &gain});
        named.add({"mute", &mute});
        named.add({"notes", &notes});
        named.add({"footer", &footer});

        addChildren({title, sidebar, gain, mute, notes, footer, status});

        auto appendOutput = [this](std::string_view chunk) { appendLog(chunk); };
        auto buildFinished = [this](const Processes::ProcessResult& result)
        { finishBuild(result); };
        runner.onOutput = appendOutput;
        runner.onFinished = buildFinished;

        reloadLayout();
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
        runner.run();
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
        if (!library.reload())
        {
            status.setText("load failed");
            Console::print("load failed: " + library.getBuiltPath().str());
            return;
        }

        layout = library.findFunction<LayoutFunction>(layoutFunctionName);

        if (layout == nullptr)
        {
            status.setText("load failed: no " + std::string {layoutFunctionName});
            return;
        }

        Console::print("generation " + std::to_string(library.getGeneration()));
        applyLayout();
    }

    void placeStatus()
    {
        auto area = getLocalBounds().inset(statusPadding);
        status.setBounds(area.removeFromBottom(statusHeight));
    }

    UI::Component* findNamed(const char* key)
    {
        for (auto& entry: named)
            if (entry.key == key)
                return entry.component;

        return nullptr;
    }

    void applyLayout()
    {
        if (layout == nullptr)
            return;

        auto entries = Vector<LayoutEntry>(layoutCapacity);
        auto table = LayoutTable {entries.data(), layoutCapacity, 0};
        auto bounds = getLocalBounds();
        layout(bounds.w, bounds.h, &table);

        auto unknown = 0;

        for (auto index = 0; index < table.count; ++index)
        {
            auto& entry = entries[index];

            if (auto* component = findNamed(entry.key))
                component->setBounds({entry.x, entry.y, entry.width, entry.height});
            else
                ++unknown;
        }

        showLoaded(unknown);
    }

    void showLoaded(int unknown)
    {
        auto text = "generation " + std::to_string(library.getGeneration())
                    + " · watching " + std::string {EACP_HOT_RELOAD_SOURCE_DIR};

        if (unknown > 0)
            text += " · " + std::to_string(unknown) + " unknown keys";

        status.setText(text);
    }

    UI::Label title {"Hot-reloaded layout"};
    Sidebar sidebar;
    UI::Slider gain;
    UI::Checkbox mute {"Mute"};
    UI::TextEditor notes;
    UI::Label footer {"Edit Layout.cpp and save to move everything above"};
    UI::Label status;

    Vector<NamedComponent> named;
    std::string log;
    LayoutFunction layout = nullptr;

    Plugins::ReloadableLibrary library {FilePath {EACP_HOT_RELOAD_LIBRARY},
                                        FilePath::appCacheDirectory() / "HotReload"};
    Processes::BuildRunner runner {makeBuildCommand()};
    Files::FileWatcher watcher {{FilePath {EACP_HOT_RELOAD_SOURCE_DIR}},
                                [this](const Vector<FilePath>&)
                                { sourcesChanged(); },
                                makeWatcherOptions()};
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
