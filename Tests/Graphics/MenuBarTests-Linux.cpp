#include "Common.h"

#include <eacp/Graphics/Menu/LinuxMenuBar-Linux.h>
#include <eacp/Graphics/Menu/MenuCommands.h>
#include <eacp/Graphics/Window/LinuxWindowNative-Linux.h>

// The window half of the Linux menu bar: where the content goes, what size the
// app is told, where pointer and key events are routed, and which shortcuts
// run. The bar itself is eacp-ui's to draw, so a stand-in presenter takes its
// place here: a plain view of a fixed height that can be told it is open. The
// windows are headless, which lays out and routes exactly as a shown one does.

using namespace nano;
using namespace eacp::Graphics;

namespace LinuxMenuBarTests
{
namespace
{
using eacp::Callback;

constexpr auto barHeight = 30.f;

struct RecordingView final : View
{
    RecordingView() { getProperties().handlesMouseEvents = true; }

    void mouseDown(const MouseEvent& event) override { downs.add(event.pos); }
    void mouseUp(const MouseEvent&) override { ++ups; }
    void keyDown(const KeyEvent&) override { ++keys; }

    eacp::Vector<Point> downs;
    int ups = 0;
    int keys = 0;
};

struct StandInPresenter final : LinuxMenuBarPresenter
{
    StandInPresenter(const MenuBar& barToUse, Callback layoutChangedToUse)
        : menus(barToUse)
        , layoutChanged(std::move(layoutChangedToUse))
    {
    }

    View& getView() override { return view; }
    float getHeight() const override { return barHeight; }

    void layout(Rect windowBounds) override
    {
        view.setBounds(
            {0.f, 0.f, windowBounds.w, open ? windowBounds.h : barHeight});
    }

    bool isMenuOpen() const override { return open; }

    void dismissMenu() override
    {
        ++dismissals;
        setOpen(false);
    }

    bool keyPressed(const KeyEvent&) override
    {
        if (open)
            ++menuKeys;

        return open;
    }

    void setOpen(bool shouldBeOpen)
    {
        open = shouldBeOpen;
        layoutChanged();
    }

    MenuBar menus;
    Callback layoutChanged;
    RecordingView view;
    bool open = false;
    int dismissals = 0;
    int menuKeys = 0;
};

// Installs the stand-in for the length of a case, and puts back no presenter
// at all, which is what GraphicsTests has without eacp-ui.
struct StandInFactory
{
    StandInFactory()
    {
        setLinuxMenuBarPresenterFactory(
            [this](const MenuBar& menus,
                   Callback layoutChanged) -> std::unique_ptr<LinuxMenuBarPresenter>
            {
                auto made = std::make_unique<StandInPresenter>(
                    menus, std::move(layoutChanged));
                last = made.get();
                return made;
            });
    }

    ~StandInFactory() { setLinuxMenuBarPresenterFactory({}); }

    StandInPresenter* last = nullptr;
};

WindowOptions sized(int width, int height)
{
    auto options = WindowOptions {};
    options.width = width;
    options.height = height;

    return options;
}

MenuBar fileMenuBar(int& saves, bool& canSave)
{
    auto file = Menu {"File"};
    file.add(MenuItem::withAction(
        "Save",
        [&saves] { ++saves; },
        commandKey("s"),
        [&canSave] { return canSave; }));

    auto bar = MenuBar {};
    bar.add(std::move(file));

    return bar;
}

MouseEvent pointer(MouseEventType type, Point position)
{
    auto event = MouseEvent {};
    event.type = type;
    event.pos = position;
    event.downPos = position;

    return event;
}

KeyEvent ctrlPress(std::string key)
{
    auto event = KeyEvent {};
    event.type = KeyEventType::Down;
    event.characters = key;
    event.charactersIgnoringModifiers = std::move(key);
    event.modifiers.control = true;

    return event;
}

LinuxWindowSurface& surfaceOf(Window& window)
{
    return linuxWindowNative(window).getWindowSurface();
}
} // namespace

// The bar adds to the window: the content keeps the size the app asked for,
// laid out below it, and that is the size the window reports.
auto tContentKeepsItsSizeUnderTheBar =
    test("LinuxMenuBar/contentKeepsItsSizeUnderTheBar") = []
{
    auto factory = StandInFactory {};
    auto content = RecordingView {};
    auto window = Window {content, sized(300, 200)};

    auto saves = 0;
    auto canSave = true;
    setApplicationMenuBar(fileMenuBar(saves, canSave), window);

    check(factory.last != nullptr);
    check(same(window.getSize(), {300.f, 200.f}));
    check(same(content.getBounds(), {0.f, barHeight, 300.f, 200.f}));
    check(same(factory.last->view.getBounds(), {0.f, 0.f, 300.f, barHeight}));
    check(same(surfaceOf(window).contentSize, {300.f, 200.f + barHeight}));

    // Views inside the content are placed in the window below the bar.
    auto child = RecordingView {};
    child.setBounds({5.f, 5.f, 10.f, 10.f});
    content.addSubview(child);

    check(same(linuxViewOriginInWindow(child), {5.f, 5.f + barHeight}));
};

auto tSetSizeIsTheContentsSize = test("LinuxMenuBar/setSizeIsTheContentsSize") = []
{
    auto factory = StandInFactory {};
    auto content = RecordingView {};
    auto options = sized(300, 200);

    auto reported = Point {};
    options.onResize = [&reported](int width, int height)
    { reported = {(float) width, (float) height}; };

    auto window = Window {content, options};

    auto saves = 0;
    auto canSave = true;
    setApplicationMenuBar(fileMenuBar(saves, canSave), window);

    window.setSize({400.f, 250.f});

    check(same(window.getSize(), {400.f, 250.f}));
    check(same(reported, {400.f, 250.f}));
    check(same(content.getBounds(), {0.f, barHeight, 400.f, 250.f}));
    check(same(factory.last->view.getBounds(), {0.f, 0.f, 400.f, barHeight}));
};

// A second bar replaces the first, and the window changes only by what the
// heights differ by, which here is nothing.
auto tReplacingTheBarKeepsTheSize =
    test("LinuxMenuBar/replacingTheBarKeepsTheSize") = []
{
    auto factory = StandInFactory {};
    auto content = RecordingView {};
    auto window = Window {content, sized(300, 200)};

    auto saves = 0;
    auto canSave = true;
    setApplicationMenuBar(fileMenuBar(saves, canSave), window);
    auto* first = factory.last;

    setApplicationMenuBar(fileMenuBar(saves, canSave), window);

    check(factory.last != first);
    check(surfaceOf(window).menuBarView == &factory.last->view);
    check(same(window.getSize(), {300.f, 200.f}));
    check(same(content.getBounds(), {0.f, barHeight, 300.f, 200.f}));
};

// A bar with nothing to show takes no room, and an app with no eacp-ui to draw
// one gets none either; both still bind their shortcuts.
auto tNothingToShowTakesNoRoom = test("LinuxMenuBar/nothingToShowTakesNoRoom") = []
{
    auto content = RecordingView {};
    auto window = Window {content, sized(300, 200)};

    auto saves = 0;
    auto canSave = true;
    setApplicationMenuBar(fileMenuBar(saves, canSave), window);

    check(surfaceOf(window).menuBarView == nullptr);
    check(same(content.getBounds(), {0.f, 0.f, 300.f, 200.f}));

    surfaceOf(window).dispatchKeyEvent(ctrlPress("s"));
    check(saves == 1);
    check(content.keys == 0);

    auto factory = StandInFactory {};
    auto bar = MenuBar {};
    bar.add(standardEditMenu());
    setApplicationMenuBar(bar, window);

    check(factory.last == nullptr);
    check(same(window.getSize(), {300.f, 200.f}));
};

auto tPointerGoesWhereItIs = test("LinuxMenuBar/pointerGoesWhereItIs") = []
{
    auto factory = StandInFactory {};
    auto content = RecordingView {};
    auto window = Window {content, sized(300, 200)};

    auto saves = 0;
    auto canSave = true;
    setApplicationMenuBar(fileMenuBar(saves, canSave), window);

    auto& surface = surfaceOf(window);
    auto& bar = factory.last->view;

    surface.dispatchMouseEvent(pointer(MouseEventType::Down, {10.f, 10.f}));
    surface.dispatchMouseEvent(pointer(MouseEventType::Up, {10.f, 10.f}));

    check(bar.downs.size() == 1 && same(bar.downs[0], {10.f, 10.f}));
    check(bar.ups == 1);
    check(content.downs.empty());

    // In the content's own coordinates, which start under the bar.
    surface.dispatchMouseEvent(pointer(MouseEventType::Down, {10.f, 50.f}));

    check(content.downs.size() == 1
          && same(content.downs[0], {10.f, 50.f - barHeight}));

    // A press keeps its view: released over the bar, it is still the content's.
    surface.dispatchMouseEvent(pointer(MouseEventType::Up, {10.f, 5.f}));

    check(content.ups == 1);
    check(bar.ups == 1);
};

// Open, the bar covers what it draws in and takes the pointer there; a press
// anywhere else closes it and reaches nothing.
auto tAnOpenMenuIsModal = test("LinuxMenuBar/anOpenMenuIsModal") = []
{
    auto factory = StandInFactory {};
    auto content = RecordingView {};
    auto window = Window {content, sized(300, 200)};

    auto saves = 0;
    auto canSave = true;
    setApplicationMenuBar(fileMenuBar(saves, canSave), window);

    auto& surface = surfaceOf(window);
    auto& presenter = *factory.last;

    presenter.setOpen(true);

    check(surface.menuBarModal);
    check(same(presenter.view.getBounds(), {0.f, 0.f, 300.f, 200.f + barHeight}));

    surface.dispatchMouseEvent(pointer(MouseEventType::Down, {10.f, 100.f}));
    check(presenter.view.downs.size() == 1);

    presenter.view.setBounds({0.f, 0.f, 300.f, 80.f});
    surface.dispatchMouseEvent(pointer(MouseEventType::Up, {10.f, 100.f}));
    surface.dispatchMouseEvent(pointer(MouseEventType::Down, {10.f, 150.f}));
    surface.dispatchMouseEvent(pointer(MouseEventType::Up, {10.f, 150.f}));

    check(presenter.dismissals == 1);
    check(!surface.menuBarModal);
    check(content.downs.empty());
    check(content.ups == 0);
    check(same(presenter.view.getBounds(), {0.f, 0.f, 300.f, barHeight}));
};

// A menu has no grab to keep it open once the user is in another window.
auto tLosingFocusClosesTheMenu = test("LinuxMenuBar/losingFocusClosesTheMenu") = []
{
    auto factory = StandInFactory {};
    auto content = RecordingView {};
    auto window = Window {content, sized(300, 200)};

    auto saves = 0;
    auto canSave = true;
    setApplicationMenuBar(fileMenuBar(saves, canSave), window);

    auto& state = linuxWindowNative(window).getState();

    state.setActive(true);
    factory.last->setOpen(true);
    state.setActive(false);

    check(factory.last->dismissals == 1);
    check(!surfaceOf(window).menuBarModal);
};

// Shortcuts run before the content sees the key, Ctrl standing for command;
// a greyed item, a release and an unbound key all go on to the content.
auto tShortcutsComeFirst = test("LinuxMenuBar/shortcutsComeFirst") = []
{
    auto factory = StandInFactory {};
    auto content = RecordingView {};
    auto window = Window {content, sized(300, 200)};

    auto saves = 0;
    auto canSave = true;
    setApplicationMenuBar(fileMenuBar(saves, canSave), window);

    auto& surface = surfaceOf(window);

    surface.dispatchKeyEvent(ctrlPress("s"));
    check(saves == 1);
    check(content.keys == 0);

    canSave = false;
    surface.dispatchKeyEvent(ctrlPress("s"));
    check(saves == 1);
    check(content.keys == 1);

    canSave = true;
    surface.dispatchKeyEvent(ctrlPress("d"));
    check(content.keys == 2);

    auto super = ctrlPress("s");
    super.modifiers.control = false;
    super.modifiers.command = true;
    surface.dispatchKeyEvent(super);
    check(saves == 1);
    check(content.keys == 3);
};

// While a menu is open it takes every key, shortcuts included.
auto tAnOpenMenuTakesTheKeys = test("LinuxMenuBar/anOpenMenuTakesTheKeys") = []
{
    auto factory = StandInFactory {};
    auto content = RecordingView {};
    auto window = Window {content, sized(300, 200)};

    auto saves = 0;
    auto canSave = true;
    setApplicationMenuBar(fileMenuBar(saves, canSave), window);

    factory.last->setOpen(true);
    surfaceOf(window).dispatchKeyEvent(ctrlPress("s"));

    check(factory.last->menuKeys == 1);
    check(saves == 0);
    check(content.keys == 0);
};

// A shortcut whose action installs another bar destroys the one it came from
// while it runs.
auto tAShortcutMayReplaceTheBar = test("LinuxMenuBar/aShortcutMayReplaceTheBar") = []
{
    auto factory = StandInFactory {};
    auto content = RecordingView {};
    auto window = Window {content, sized(300, 200)};

    auto replaced = 0;
    auto file = Menu {"File"};

    file.add(MenuItem::withAction(
        "Reload",
        [&]
        {
            ++replaced;

            auto other = MenuBar {};
            auto menu = Menu {"Other"};
            menu.add(MenuItem::withAction("Nothing"));
            other.add(std::move(menu));

            setApplicationMenuBar(other, window);
        },
        commandKey("r")));

    auto bar = MenuBar {};
    bar.add(std::move(file));
    setApplicationMenuBar(bar, window);

    surfaceOf(window).dispatchKeyEvent(ctrlPress("r"));

    check(replaced == 1);
    check(factory.last->menus.menus[0].title == "Other");
    check(same(window.getSize(), {300.f, 200.f}));
};

auto tStandardMenuIsFileWithQuit =
    test("LinuxMenuBar/standardMenuIsFileWithQuit") = []
{
    auto menu = standardApplicationMenu("Some App");

    check(menu.title == "File");
    check(menu.items.size() == 1);
    check(menu.items[0].title == "Quit");
    check(menu.items[0].shortcut.has_value());
    check(acceleratorText(*menu.items[0].shortcut) == "Ctrl+Q");
};
} // namespace LinuxMenuBarTests
