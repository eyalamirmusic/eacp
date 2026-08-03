#include <eacp/UI/UI.h>

#include <algorithm>

using namespace eacp;
using namespace Graphics;

// A smooth orange disc, generated so the example needs no asset file. On
// macOS the menu bar renders it as a template (alpha-only, system tinted);
// on Windows the colour shows in the notification area.
static Image makeTrayIcon()
{
    constexpr int size = 36;
    auto image = Image(size, size);

    auto center = (size - 1) / 2.f;
    auto radius = size * 0.42f;

    for (auto y = 0; y < size; ++y)
    {
        for (auto x = 0; x < size; ++x)
        {
            auto dx = static_cast<float>(x) - center;
            auto dy = static_cast<float>(y) - center;
            auto distance = std::sqrt(dx * dx + dy * dy);

            auto coverage = std::clamp(radius - distance, 0.f, 1.f);
            if (coverage <= 0.f)
                continue;

            image.set(x, y, Color(0.95f, 0.55f, 0.1f, coverage));
        }
    }

    return image;
}

// The content of the floating panel below. The window's cornerRadius clips
// this component tree, so it just fills its bounds — the rounding comes for
// free.
struct PanelContent final : UI::Component
{
    PanelContent()
    {
        title.setFontSize(17.f);
        title.setColour({0.95f, 0.95f, 0.95f, 1.f});

        subtitle.setColour({0.62f, 0.62f, 0.68f, 1.f});

        addChildren({title, subtitle});
    }

    void paint(UI::Graphics& g) override { g.fillAll({0.11f, 0.11f, 0.13f, 1.f}); }

    void resized() override
    {
        auto area = getLocalBounds().inset(20.f, 24.f);

        title.setBounds(area.removeFromTop(26.f));
        subtitle.setBounds(area.removeFromTop(22.f));
    }

    UI::Label title {"Quick Panel"};
    UI::Label subtitle {"Toggled from the tray, never recreated"};
};

struct PanelHost final : UI::ComponentHost
{
    PanelHost()
    {
        setBackgroundColour({0.11f, 0.11f, 0.13f, 1.f});
        setRootComponent(content);
    }

    PanelContent content;
};

// The main window's content: a written-out description of the three ways to
// put this app away, because the whole point of the window is that two of
// them keep it running and one does not.
struct MainContent final : UI::Component
{
    MainContent()
    {
        title.setFontSize(19.f);
        title.setColour({0.95f, 0.95f, 0.95f, 1.f});

        for (auto* line: {&closeLine, &quitLine, &trayLine})
            line->setColour({0.62f, 0.62f, 0.68f, 1.f});

        addChildren({title, closeLine, quitLine, trayLine});
    }

    void paint(UI::Graphics& g) override { g.fillAll({0.11f, 0.11f, 0.13f, 1.f}); }

    void resized() override
    {
        auto area = getLocalBounds().inset(24.f, 26.f);

        title.setBounds(area.removeFromTop(30.f));
        area.removeFromTop(8.f);

        for (auto* line: {&closeLine, &quitLine, &trayLine})
            line->setBounds(area.removeFromTop(22.f));
    }

    UI::Label title {"Tray App"};
    UI::Label closeLine {"Red button: Dock icon goes, tray icon stays"};
    UI::Label quitLine {"Cmd+Q / Dock ▸ Quit: the same, not an exit"};
    UI::Label trayLine {"Tray ▸ Quit: the only way out"};
};

struct MainHost final : UI::ComponentHost
{
    MainHost()
    {
        setBackgroundColour({0.11f, 0.11f, 0.13f, 1.f});
        setRootComponent(content);
    }

    MainContent content;
};

struct TrayApp
{
    TrayApp()
    {
        // The panel shows itself on construction; hide it immediately so the
        // app starts with the main window only. setVisible keeps the window
        // (and its content) alive across toggles, so it reappears exactly
        // where the user left it.
        window.setContentView(panelHost);
        window.setVisible(false);

        mainWindow.setContentView(mainHost);

        // The Cmd+Q the demo below refuses lives on this menu — without a
        // main menu the key equivalent has nothing to fire.
        auto bar = MenuBar {};
        bar.add(standardApplicationMenu("Tray App"));
        setApplicationMenuBar(bar, mainWindow);

        // hidesOnClose has already ordered the window out by the time this
        // fires, and it is the only sign app code gets that the user closed
        // it — onQuit is exactly what hidesOnClose suppresses. So this is
        // where the app steps back down to being a bare tray icon. The icon
        // alone: hiding the window again from inside its own close is both
        // redundant and a re-entrant call into the delegate mid-event.
        mainWindow.events.onHidden = [] { Apps::setDockIconVisible(false); };

        // Cmd+Q, the app menu's Quit and Dock ▸ Quit are the same gesture as
        // the red button for an app that lives in the tray: put it away, do
        // not exit. Refusing here answers all three at once.
        //
        // The tray's own Quit calls Apps::quit(), which bypasses this
        // handler — and must, or an app refusing every request would have no
        // way out at all.
        Apps::setQuitHandler(
            [this]
            {
                showMainWindow(false);
                return false;
            });

        tray.setIcon(makeTrayIcon());
        tray.setTooltip("eacp Tray App");

        tray.setMenu(createTrayMenu());

        // Windows: a left-click on the tray icon toggles the panel (the
        // menu stays on right-click). On macOS the menu owns the click, so
        // this never fires there — use the menu item instead.
        tray.setOnClick([this] { togglePanel(); });

        // The bundle is LSUIElement, so the app launched with no Dock icon
        // and the main window is already on screen — promote it to a regular
        // Dock app to match, with no icon flash on the way.
        showMainWindow(true);
    }

    // The ordinary window the Dock icon belongs to. hidesOnClose is what
    // makes the red button survivable: the window orders out with its state
    // intact instead of being destroyed, and the app keeps running.
    static WindowOptions getMainWindowOptions()
    {
        auto options = WindowOptions();

        options.width = 460;
        options.height = 220;
        options.title = "Tray App";
        options.hidesOnClose = true;

        return options;
    }

    // A small tray companion: borderless and rounded (cornerRadius defines
    // the shape of a frameless window), floating above normal windows,
    // following the user across Spaces, and shown without stealing focus
    // from whatever they're working in.
    static WindowOptions getPanelOptions()
    {
        auto options = WindowOptions();

        options.width = 320;
        options.height = 180;
        options.isPrimary = false;

        options.flags = {WindowFlags::Borderless};
        options.cornerRadius = 14.f;

        options.alwaysOnTop = true;
        options.visibleOnAllWorkspaces = true;
        options.showInactive = true;

        return options;
    }

    Menu createTrayMenu()
    {
        auto menu = Menu();

        // Once the Dock icon is gone this is the only way back — which is
        // the deal a tray-resident app makes when it refuses to quit.
        menu.add(
            MenuItem::withAction("Open Tray App", [this] { showMainWindow(true); }));
        menu.add(MenuItem::withAction("Toggle Panel", [this] { togglePanel(); }));
        menu.add(
            MenuItem::withAction("Say Hello", [] { LOG("Hello from the tray!"); }));
        menu.addSeparator();

        // quit(), not requestQuit(): the handler above refuses every request,
        // and this item is the exit it is refusing them in favour of.
        menu.add(MenuItem::withAction("Quit", [] { Apps::quit(); }));
        return menu;
    }

    // The Dock icon follows the main window, so the app is a regular Dock app
    // exactly while it has a window to show and a bare tray icon otherwise.
    void showMainWindow(bool shouldShow)
    {
        Apps::setDockIconVisible(shouldShow);
        mainWindow.setVisible(shouldShow);

        if (shouldShow)
            mainWindow.toFront();
    }

    void togglePanel() { window.setVisible(!window.isVisible()); }

    PanelHost panelHost;
    MainHost mainHost;
    Window window {getPanelOptions()};
    Window mainWindow {getMainWindowOptions()};
    TrayIcon tray;
};

int main()
{
    return eacp::Apps::run<TrayApp>();
}
