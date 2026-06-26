#include <eacp/Graphics/Graphics.h>

#include <optional>

using namespace eacp;
using namespace Graphics;

static constexpr ModifierKeys launcherModifiers {.shift = true, .control = true};
static constexpr uint16_t launcherKeyCode = KeyCode::L;

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

struct PanelView final : View
{
    PanelView()
        : textInput(FontOptions().withName("Helvetica").withSize(18.f))
    {
        background->setFillColor({0.11f, 0.11f, 0.13f});

        textInput.setPlaceholder("What can I help you with today?");
        textInput.setTextColor({0.95f, 0.95f, 0.95f});
        textInput.setBackgroundColor({0.11f, 0.11f, 0.13f});
        textInput.setBorderColor({0.f, 0.f, 0.f, 0.f});
        textInput.setPadding(18.f);

        addChildren({background});
        addSubview(textInput);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto path = Path();
        path.addRect(bounds);
        background->setPath(path);

        scaleToFit({background});
        textInput.setBounds({0.f, 0.f, bounds.w, bounds.h});
    }

    ShapeLayerView background;
    TextInput textInput;
};

struct TrayApp
{
    TrayApp()
    {
        Apps::setDockIconVisible(false);
        Apps::setLaunchAtLogin(true);

        panelView.textInput.onSubmit([this](const std::string&) { swallowAndHide(); });

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

        hotKey.emplace(launcherModifiers, launcherKeyCode, [this] { showPanel(); });
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
        menu.addSeparator();
        menu.add(MenuItem::withAction("Quit", [] { Apps::quit(); }));
        return menu;
    }

    void showPanel()
    {
        window.setVisible(true);
        window.toFront();
        panelView.textInput.focus();
    }

    void hidePanel() { window.setVisible(false); }

    void swallowAndHide()
    {
        panelView.textInput.setText("");
        hidePanel();
    }

    void togglePanel()
    {
        if (window.isVisible())
            hidePanel();
        else
            showPanel();
    }

    PanelView panelView;
    Window window {getPanelOptions()};
    TrayIcon tray;
    std::optional<GlobalHotKey> hotKey;
};

int main()
{
    eacp::Apps::run<TrayApp>();
    return 0;
}
