#include <eacp/Graphics/Graphics.h>
#include <eacp/Graphics/Helpers/SystemAppearance.h>
#include <eacp/SVG/SVG.h>

using namespace eacp;
using namespace Graphics;

static constexpr std::string_view logoSvgMarkup()
{
    return R"SVG(

<svg width="585" height="510" viewBox="0 0 585 510" fill="none" xmlns="http://www.w3.org/2000/svg">
    <path d="M193.823 193.365V27H27V193.365" stroke="black" stroke-width="54"/>
    <path d="M376.159 509.48V426.527C376.159 380.46 338.814 343.115 292.747 343.115C246.681 343.115 209.336 380.46 209.336 426.527V509.48" stroke="black" stroke-width="54"/>
    <path d="M558 193.365V27H391.177V193.365" stroke="black" stroke-width="54"/>
    <path d="M197.445 13.622L292.413 182.393L387.709 13.622" stroke="black" stroke-width="54" stroke-linejoin="bevel"/>
    <path d="M193.723 192.984C193.723 247.439 237.979 291.584 292.571 291.584C347.163 291.584 391.177 247.687 391.177 193.231" stroke="black" stroke-width="54"/>
</svg>

)SVG";
}

// The logo SVG rasterized at tray size and centred on a square transparent
// canvas (the art is slightly wider than tall). The rendered strokes supply
// coverage through their alpha; the colour follows the system theme, which
// is what Windows shows in the notification area. macOS ignores the colour
// and tints the template icon from alpha by itself.
static Image makeTrayIcon()
{
    constexpr auto size = 36;

    auto logo = SVG::toImage(std::string(logoSvgMarkup()), size);
    auto icon = Image(size, size);

    auto offsetX = (size - logo.width()) / 2;
    auto offsetY = (size - logo.height()) / 2;
    auto tint = isSystemDarkMode() ? Color::white() : Color::black();

    for (auto y = 0; y < logo.height(); ++y)
        for (auto x = 0; x < logo.width(); ++x)
            icon.set(offsetX + x, offsetY + y, tint.withAlpha(logo.at(x, y).a));

    return icon;
}

// The content of the floating panel below. The window's cornerRadius clips
// this view, so it just fills its bounds — the rounding comes for free.
struct PanelView final : View
{
    PanelView()
    {
        background->setFillColor({0.11f, 0.11f, 0.13f});
        title->setColor({0.95f, 0.95f, 0.95f});
        subtitle->setColor({0.62f, 0.62f, 0.68f});

        addChildren({background, title, subtitle});
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto path = Path();
        path.addRect(bounds);
        background->setPath(path);

        scaleToFit({background, title, subtitle});
        title->setPosition({20.f, bounds.h - 44.f});
        subtitle->setPosition({20.f, bounds.h - 70.f});
    }

    ShapeLayerView background;
    TextLayerView title {"Quick Panel"};
    TextLayerView subtitle {"Toggled from the tray, never recreated"};
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
        window.setContentView(panelView);
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

    PanelView panelView;
    Window window {getPanelOptions()};
    TrayIcon tray;
};

int main()
{
    eacp::Apps::run<TrayApp>();
    return 0;
}
