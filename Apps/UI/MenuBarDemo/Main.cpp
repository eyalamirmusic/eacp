#include <eacp/UI/UI.h>

#include <string>

// One Graphics::MenuBar handed to setApplicationMenuBar, and the state its
// items read and change printed in the window.
//
// The same bar is the system's menu bar on macOS and the window's on Windows;
// on Linux it is drawn in the window, above the content, which keeps the size
// the window was asked for. Watch the items follow the counter without the bar
// ever being rebuilt: Reset greys out at zero, the step in use is ticked, and
// Show Grid ticks as the grid comes and goes. Each shortcut (Ctrl+I, Ctrl+G,
// Ctrl+Q on Linux and Windows; Cmd on macOS) does what its item does.

using namespace eacp;

namespace
{
struct DemoState
{
    int counter = 0;
    int step = 1;
    bool grid = true;
    std::string lastCommand = "none yet";
};

struct Content final : UI::Component
{
    explicit Content(DemoState& stateToUse)
        : state(stateToUse)
    {
        heading.setFontSize(18.f);
        hint.setThemeColour(UI::ThemeKeys::dimText);
        addChildren({heading, counter, command, hint});
    }

    void refresh()
    {
        counter.setText("Counter: " + std::to_string(state.counter) + "  (step "
                        + std::to_string(state.step) + ")");
        command.setText("Last command: " + state.lastCommand);
        repaint();
    }

    void paint(UI::Graphics& g) override
    {
        const auto& theme = getHostTheme();

        g.fillAll(theme.get(UI::ThemeKeys::background));

        if (!state.grid)
            return;

        g.setColour(theme.get(UI::ThemeKeys::outline));

        for (auto x = 0.f; x < getWidth(); x += 32.f)
            g.drawLine({x, 0.f}, {x, getHeight()});

        for (auto y = 0.f; y < getHeight(); y += 32.f)
            g.drawLine({0.f, y}, {getWidth(), y});
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(24.f);

        heading.setBounds(area.removeFromTop(32.f));
        counter.setBounds(area.removeFromTop(28.f));
        command.setBounds(area.removeFromTop(28.f));
        hint.setBounds(area.removeFromTop(28.f));
    }

    DemoState& state;
    UI::Label heading {"The menu bar drives this window"};
    UI::Label counter;
    UI::Label command;
    UI::Label hint {"Ctrl+I counts, Ctrl+G toggles the grid"};
};

struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        setRootComponent(content);
        content.refresh();
    }

    DemoState state;
    Content content {state};
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 640;
    options.height = 400;
    options.title = "eacp UI - menu bar";

    return options;
}

struct App
{
    App() { installMenuBar(); }

    void did(std::string command)
    {
        host.state.lastCommand = std::move(command);
        host.content.refresh();
    }

    void installMenuBar()
    {
        using Graphics::commandKey;
        using Graphics::MenuItem;

        auto& state = host.state;

        auto steps = Graphics::Menu {"Step"};

        for (auto step: {1, 5, 10})
            steps.add(MenuItem::withCheckableAction(
                "By " + std::to_string(step),
                [this, &state, step]
                {
                    state.step = step;
                    did("Step by " + std::to_string(step));
                },
                [&state, step] { return state.step == step; }));

        auto counter = Graphics::Menu {"Counter"};

        counter.add(MenuItem::withAction(
            "Increment",
            [this, &state]
            {
                state.counter += state.step;
                did("Increment");
            },
            commandKey("i")));

        counter.add(MenuItem::withAction(
            "Reset",
            [this, &state]
            {
                state.counter = 0;
                did("Reset");
            },
            {},
            [&state] { return state.counter > 0; }));

        counter.addSeparator();
        counter.add(MenuItem::withSubmenu("Step", std::move(steps)));

        auto view = Graphics::Menu {"View"};

        view.add(MenuItem::withCheckableAction(
            "Show Grid",
            [this, &state]
            {
                state.grid = !state.grid;
                did(state.grid ? "Show Grid" : "Hide Grid");
            },
            [&state] { return state.grid; },
            commandKey("g")));

        auto bar = Graphics::MenuBar {};
        bar.add(Graphics::standardApplicationMenu("MenuBarDemo"));
        bar.add(std::move(counter));
        bar.add(std::move(view));

        Graphics::setApplicationMenuBar(bar, window);
    }

    DemoHost host;
    Graphics::Window window {host, makeOptions()};
};
} // namespace

int main()
{
    return eacp::Apps::run<App>();
}
