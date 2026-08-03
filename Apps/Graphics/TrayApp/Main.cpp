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

        addAndMakeVisible(title);
        addAndMakeVisible(subtitle);
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

struct TrayApp
{
    TrayApp()
    {
        Apps::setDockIconVisible(false);

        // The window shows itself on construction; hide it immediately so
        // the app starts as a bare tray icon. setVisible keeps the window
        // (and its content) alive across toggles, so it reappears exactly
        // where the user left it.
        window.setContentView(panelHost);
        window.setVisible(false);

        tray.setIcon(makeTrayIcon());
        tray.setTooltip("eacp Tray App");

        tray.setMenu(createTrayMenu());

        // Windows: a left-click on the tray icon toggles the panel (the
        // menu stays on right-click). On macOS the menu owns the click, so
        // this never fires there — use the menu item instead.
        tray.setOnClick([this] { togglePanel(); });
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
        menu.add(MenuItem::withAction("Toggle Panel", [this] { togglePanel(); }));
        menu.add(
            MenuItem::withAction("Say Hello", [] { LOG("Hello from the tray!"); }));
        menu.addSeparator();
        menu.add(MenuItem::withAction("Quit", [] { Apps::quit(); }));
        return menu;
    }

    void togglePanel() { window.setVisible(!window.isVisible()); }

    PanelHost panelHost;
    Window window {getPanelOptions()};
    TrayIcon tray;
};

int main()
{
    return eacp::Apps::run<TrayApp>();
}
