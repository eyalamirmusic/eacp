#include <eacp/UI/UI.h>

#include <string>

using namespace eacp;
using namespace Graphics;

// Menu items that grey themselves out from live application state.
//
// The point of the example is what you cannot see in a screenshot: nothing here
// rebuilds the menu bar. It is installed once, at startup, and every item that
// carries an `isEnabled` predicate is asked afresh each time the menu opens. So
// open the Demo menu, close it, change the state with the window's keys, and
// open it again — the greying follows without the app touching the bar.
//
// Without that, an app has two bad options: rebuild the whole bar on every
// state change (AppKit may be tracking it at the time), or let items advertise
// commands that quietly do nothing.

struct StateContent final : UI::Component
{
    StateContent()
    {
        title.setFontStyle(UI::FontStyle::Bold);
        title.setColour({0.95f, 0.95f, 0.95f, 1.f});

        for (auto* line: {&documentLine, &counterLine, &hintLine})
            line->setColour({0.68f, 0.68f, 0.74f, 1.f});

        // The window's keys are this component's, so it says it wants them.
        // Nothing else in the tree does, which is why nothing else has to.
        setWantsKeyboardFocus(true);
        setInterceptsMouseClicks(true);

        addChildren({title, documentLine, counterLine, hintLine});

        refresh();
    }

    // Space toggles the document open, Up/Down move the counter — the two bits
    // of state the Demo menu reads. Anything else is returned unconsumed, so the
    // keys this window does not use are still the window's.
    bool keyDown(const UI::KeyEvent& event) override
    {
        if (event.keyCode == UI::KeyCode::Space)
            documentOpen = !documentOpen;
        else if (event.keyCode == UI::KeyCode::UpArrow)
            ++counter;
        else if (event.keyCode == UI::KeyCode::DownArrow && counter > 0)
            --counter;
        else
            return false;

        refresh();

        return true;
    }

    void refresh()
    {
        documentLine.setText(documentOpen ? "Document: open" : "Document: closed");
        counterLine.setText("Counter: " + std::to_string(counter));
    }

    void paint(UI::Graphics& g) override { g.fillAll({0.11f, 0.11f, 0.13f, 1.f}); }

    void resized() override
    {
        auto area = getLocalBounds().inset(24.f, 24.f);

        title.setBounds(area.removeFromTop(26.f));
        area.removeFromTop(18.f);

        documentLine.setBounds(area.removeFromTop(24.f));
        counterLine.setBounds(area.removeFromTop(24.f));
        area.removeFromTop(18.f);

        hintLine.setBounds(area.removeFromTop(24.f));
    }

    bool documentOpen = false;
    int counter = 0;

    UI::Label title {"Open the Demo menu, then change this state"};
    UI::Label documentLine;
    UI::Label counterLine;
    UI::Label hintLine {
        "Space toggles the document \u00b7 Up/Down move the counter"};
};

struct StateHost final : UI::ComponentHost
{
    StateHost()
    {
        setBackgroundColour({0.11f, 0.11f, 0.13f, 1.f});
        setRootComponent(content);
        content.grabKeyboardFocus();
    }

    StateContent content;
};

struct MenuBarApp
{
    MenuBarApp()
    {
        window.setContentView(host);
        host.focus();
        installMenuBar();
    }

    void installMenuBar()
    {
        auto demo = Menu {"Demo"};

        demo.add(MenuItem::withAction(
            "Toggle Document", [this] { toggleDocument(); }, commandKey("d")));

        demo.addSeparator();

        // The two items worth watching. Neither is ever rebuilt; both follow
        // the state the window prints.
        demo.add(MenuItem::withAction(
            "Close Document",
            [this] { toggleDocument(); },
            commandKey("w"),
            [this] { return host.content.documentOpen; }));

        demo.add(MenuItem::withAction(
            "Reset Counter",
            [this] { resetCounter(); },
            {},
            [this] { return host.content.counter > 0; }));

        demo.addSeparator();

        // Says nothing about availability, so it is always available — the case
        // every item was in before enablement existed.
        demo.add(MenuItem::withAction(
            "Increment Counter", [this] { incrementCounter(); }, commandKey("i")));

        auto bar = MenuBar {};
        bar.add(standardApplicationMenu("MenuBarApp"));
        bar.add(std::move(demo));

        setApplicationMenuBar(bar, window);
    }

    void toggleDocument()
    {
        host.content.documentOpen = !host.content.documentOpen;
        host.content.refresh();
    }

    void incrementCounter()
    {
        ++host.content.counter;
        host.content.refresh();
    }

    void resetCounter()
    {
        host.content.counter = 0;
        host.content.refresh();
    }

    StateHost host;
    Window window {[]
                   {
                       auto options = WindowOptions {};
                       options.width = 620;
                       options.height = 220;
                       options.title = "Menu Enablement";
                       options.backgroundColor = Color {0.11f, 0.11f, 0.13f};
                       return options;
                   }()};
};

int main(int argc, char* argv[])
{
    return Apps::run<MenuBarApp>(argc, argv);
}
