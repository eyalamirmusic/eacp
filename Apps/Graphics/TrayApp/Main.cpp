#include "Types.h"
#include "Clipboard.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Graphics/Graphics.h>
#include <eacp/WebView/WebView.h>

#include <optional>

using namespace eacp;
using namespace Graphics;

static constexpr ModifierKeys launcherModifiers {.alt = true, .command = true};
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

static WebView::Options getWebViewOptions()
{
    auto options = embeddedOptions("TrayApp");
    options.embedded.devServerURL = "http://localhost:5179";
    options.embedded.preferDevServer = false;
    options.embedded.autoLoad = false;
    options.acceptFirstMouse = true;
    return options;
}

struct TrayApp
{
    TrayApp()
    {
        Apps::setDockIconVisible(false);
        Apps::setLaunchAtLogin(true);

        api.onArmDrag = [this](const EA::Vector<std::string>& paths)
        { webView.armFileDrag(paths); };
        api.onCopyFiles = [](const EA::Vector<std::string>& paths)
        { return TrayAppNative::copyFilesToClipboard(paths); };
        api.onSubmit = [this](const std::string&) { swallowAndHide(); };
        api.onDismiss = [this] { hidePanel(); };
        webView.onFileDragStarted =
            [this]
            {
                Threads::callAsync([this] { hidePanel(); });
            };
        webView.onNavigationFinished =
            [this](const std::string&)
            {
                if (window.isVisible())
                    focusPrompt();
            };
        webView.loadURL("app://local/index.html");

        // The window shows itself on construction; hide it immediately so
        // the app starts as a bare tray icon. setVisible keeps the window
        // (and its content) alive across toggles, so it reappears exactly
        // where the user left it.
        window.setContentView(webView);
        window.setVisible(false);
        window.events.onActivationChanged =
            [this](bool isKey)
            {
                if (!isKey && window.isVisible())
                    hidePanel();
            };

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

        options.width = 760;
        options.height = 430;
        options.isPrimary = false;

        options.flags = {WindowFlags::Borderless};
        options.cornerRadius = 18.f;

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
        focusPrompt();
        Threads::callAsync(
            [this]
            {
                if (window.isVisible())
                    focusPrompt();
            });
    }

    void focusPrompt()
    {
        webView.focusContent();
        webView.evaluateJavaScript(
            "(() => {"
            "const focus = () => {"
            "const input = document.querySelector('input[name=\"prompt\"]');"
            "if (!input) return false;"
            "input.focus();"
            "input.select?.();"
            "return document.activeElement === input;"
            "};"
            "if (focus()) return;"
            "setTimeout(focus, 0);"
            "setTimeout(focus, 50);"
            "setTimeout(focus, 150);"
            "})();");
    }

    void hidePanel()
    {
        api.stopAudio();
        window.setVisible(false);
    }

    void swallowAndHide()
    {
        webView.evaluateJavaScript(
            "const i=document.querySelector('input[name=\"prompt\"]');"
            "if(i){i.value='';}");
        hidePanel();
    }

    void togglePanel()
    {
        if (window.isVisible())
            hidePanel();
        else
            showPanel();
    }

    Api::TrayLauncherApi api;
    WebView webView {getWebViewOptions()};
    WebViewBridge transport {webView, api};
    Window window {getPanelOptions()};
    TrayIcon tray;
    std::optional<GlobalHotKey> hotKey;
};

int main()
{
    eacp::Apps::run<TrayApp>();
    return 0;
}
