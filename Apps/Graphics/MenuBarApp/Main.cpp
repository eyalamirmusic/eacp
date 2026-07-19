#include <eacp/Graphics/Graphics.h>

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

struct StateView final : View
{
    StateView()
    {
        getProperties().handlesMouseEvents = true;
        getProperties().grabsFocusOnMouseDown = true;

        background->setFillColor({0.11f, 0.11f, 0.13f});

        title->setFont(FontOptions().withName("Helvetica-Bold"));
        title->setColor({0.95f, 0.95f, 0.95f});

        for (auto* line: {&documentLine, &counterLine, &hintLine})
            (*line)->setColor({0.68f, 0.68f, 0.74f});

        addChildren({background, title, documentLine, counterLine, hintLine});

        refresh();
    }

    // Space toggles the document open, Up/Down move the counter — the two bits
    // of state the Demo menu reads.
    void keyDown(const KeyEvent& event) override
    {
        if (event.keyCode == KeyCode::Space)
            documentOpen = !documentOpen;
        else if (event.keyCode == KeyCode::UpArrow)
            ++counter;
        else if (event.keyCode == KeyCode::DownArrow && counter > 0)
            --counter;
        else
            return;

        refresh();
    }

    void refresh()
    {
        documentLine->setText(documentOpen ? "Document: open" : "Document: closed");

        counterLine->setText("Counter: " + std::to_string(counter));
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto path = Path();
        path.addRect(bounds);
        background->setPath(path);

        scaleToFit({background, title, documentLine, counterLine, hintLine});

        // Layers are y-down, like the rest of the framework, so these run down
        // the view rather than up it.
        title->setPosition({24.f, 28.f});
        documentLine->setPosition({24.f, 72.f});
        counterLine->setPosition({24.f, 100.f});
        hintLine->setPosition({24.f, 144.f});
    }

    bool documentOpen = false;
    int counter = 0;

    ShapeLayerView background;
    TextLayerView title {"Open the Demo menu, then change this state"};
    TextLayerView documentLine;
    TextLayerView counterLine;
    TextLayerView hintLine {"Space toggles the document · Up/Down move the counter"};
};

struct MenuBarApp
{
    MenuBarApp()
    {
        window.setContentView(view);
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
            [this] { return view.documentOpen; }));

        demo.add(MenuItem::withAction(
            "Reset Counter",
            [this] { resetCounter(); },
            {},
            [this] { return view.counter > 0; }));

        demo.addSeparator();

        // Says nothing about availability, so it is always available — the case
        // every item was in before enablement existed.
        demo.add(MenuItem::withAction(
            "Increment Counter", [this] { incrementCounter(); }, commandKey("i")));

        auto bar = MenuBar {};
        bar.add(standardApplicationMenu("MenuBarApp"));
        bar.add(std::move(demo));

        setApplicationMenuBar(bar);
    }

    void toggleDocument()
    {
        view.documentOpen = !view.documentOpen;
        view.refresh();
    }

    void incrementCounter()
    {
        ++view.counter;
        view.refresh();
    }

    void resetCounter()
    {
        view.counter = 0;
        view.refresh();
    }

    StateView view;
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
