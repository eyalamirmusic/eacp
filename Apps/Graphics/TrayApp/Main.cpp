#include "Api.h"
#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Threads/Timer.h>
#include <eacp/Core/App/Clipboard.h>
#include <eacp/Graphics/Graphics.h>
#include <eacp/WebView/WebView.h>

#include <chrono>
#include <optional>
#include <string_view>

using namespace eacp;
using namespace Graphics;

static constexpr ModifierKeys launcherModifiers {.alt = true, .command = true};
static constexpr uint16_t launcherKeyCode = KeyCode::L;
static constexpr const char* searchDownloadsCommand = "searchDownloads";
static constexpr auto copyToastDuration = std::chrono::milliseconds {1500};

static std::string escapeHTML(std::string_view text)
{
    auto result = std::string {};
    result.reserve(text.size());

    for (auto c: text)
    {
        switch (c)
        {
            case '&':
                result += "&amp;";
                break;
            case '<':
                result += "&lt;";
                break;
            case '>':
                result += "&gt;";
                break;
            case '"':
                result += "&quot;";
                break;
            case '\'':
                result += "&#39;";
                break;
            default:
                result += c;
                break;
        }
    }

    return result;
}

static std::string copyToastHTML(const std::string& sampleName)
{
    return R"html(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
:root {
    color-scheme: dark;
    font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    background: transparent;
}

* { box-sizing: border-box; }

html,
body {
    width: 100%;
    height: 100%;
    margin: 0;
    overflow: hidden;
    background: transparent;
}

body {
    display: grid;
    place-items: center;
}

.hud {
    display: grid;
    justify-items: center;
    width: 300px;
    min-height: 190px;
    padding: 30px 26px 24px;
    border-radius: 32px;
    background: rgba(28, 28, 32, 0.93);
    border: 1px solid rgba(255, 255, 255, 0.08);
    box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.06);
    animation: hud-in 1500ms cubic-bezier(0.2, 0.8, 0.2, 1) both;
}

.mark {
    position: relative;
    width: 58px;
    height: 58px;
    margin-bottom: 18px;
}

.mark span {
    position: absolute;
    left: 26px;
    top: 3px;
    width: 6px;
    height: 52px;
    border-radius: 999px;
    background: #f1f1f2;
    transform-origin: 50% 50%;
}

.mark span:nth-child(2) { transform: rotate(22.5deg); }
.mark span:nth-child(3) { transform: rotate(45deg); }
.mark span:nth-child(4) { transform: rotate(67.5deg); }
.mark span:nth-child(5) { transform: rotate(90deg); }
.mark span:nth-child(6) { transform: rotate(112.5deg); }
.mark span:nth-child(7) { transform: rotate(135deg); }
.mark span:nth-child(8) { transform: rotate(157.5deg); }

.title {
    color: #f6f4f2;
    font-size: 26px;
    font-weight: 720;
    letter-spacing: 0;
    line-height: 1.1;
}

.detail {
    max-width: 100%;
    margin-top: 8px;
    color: rgba(246, 244, 242, 0.68);
    font-size: 13px;
    font-weight: 550;
    line-height: 1.28;
    overflow: hidden;
    text-align: center;
    text-overflow: ellipsis;
    white-space: nowrap;
}

@keyframes hud-in {
    0% { opacity: 0; transform: translateY(10px) scale(0.94); }
    10%, 72% { opacity: 1; transform: translateY(0) scale(1); }
    100% { opacity: 0; transform: translateY(-8px) scale(0.98); }
}
</style>
</head>
<body>
<div class="hud">
    <div class="mark" aria-hidden="true">
        <span></span><span></span><span></span><span></span>
        <span></span><span></span><span></span><span></span>
    </div>
    <div class="title">Copied</div>
    <div class="detail">)html" + escapeHTML(sampleName) + R"html( to clipboard</div>
</div>
</body>
</html>
)html";
}

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
        { return Clipboard::copyFiles(paths); };
        api.onShowCopyToast = [this](const std::string& name)
        { showCopyToast(name); };
        api.onSubmit = [this](const std::string&) { swallowAndHide(); };
        api.onDismiss = [this] { hidePanel(); };

        transport.setCommandExecution(searchDownloadsCommand,
                                      CommandExecution::WorkerThread);

        webView.onFileDragStarted = [this] { hidePanel(); };
        webView.onNavigationFinished = [this](const std::string&)
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
        window.events.onActivationChanged = [this](bool isKey)
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

        copyToastWindow.setContentView(copyToastWebView);
        copyToastWindow.setVisible(false);
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

    static WindowOptions getCopyToastOptions()
    {
        auto options = WindowOptions();

        options.width = 320;
        options.height = 220;
        options.isPrimary = false;

        options.flags = {WindowFlags::Borderless};
        options.cornerRadius = 32.f;

        options.alwaysOnTop = true;
        options.visibleOnAllWorkspaces = true;
        options.showInactive = true;
        options.ignoresMouseEvents = true;

        return options;
    }

    static WebView::Options getCopyToastWebViewOptions()
    {
        auto options = WebView::Options();
        options.embedded.autoLoad = false;
        options.debugConsole = false;
        options.transparentBackground = true;
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
        api.setPlaybackEnabled(true);
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
        webView.evaluateJavaScript(R"js(
(() => {
    const focus = () => {
        const input = document.querySelector('input[name="prompt"]');
        if (!input) return false;
        input.focus();
        input.select?.();
        return document.activeElement === input;
    };
    if (focus()) return;
    setTimeout(focus, 0);
    setTimeout(focus, 50);
    setTimeout(focus, 150);
})();
)js");
    }

    void hidePanel()
    {
        api.setPlaybackEnabled(false);
        window.setVisible(false);
    }

    void showCopyToast(const std::string& sampleName)
    {
        copyToastWebView.loadHTML(copyToastHTML(sampleName));
        copyToastVisible = true;
        copyToastDeadline = std::chrono::steady_clock::now() + copyToastDuration;
        copyToastWindow.setVisible(true);
    }

    void updateCopyToast()
    {
        if (!copyToastVisible)
            return;

        if (std::chrono::steady_clock::now() < copyToastDeadline)
            return;

        copyToastVisible = false;
        copyToastWindow.setVisible(false);
    }

    void swallowAndHide()
    {
        webView.evaluateJavaScript(R"js(
const input = document.querySelector('input[name="prompt"]');
if (input) input.value = '';
)js");
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
    WebView copyToastWebView {getCopyToastWebViewOptions()};
    Window copyToastWindow {getCopyToastOptions()};
    bool copyToastVisible = false;
    std::chrono::steady_clock::time_point copyToastDeadline {};
    Threads::Timer copyToastTimer {[this] { updateCopyToast(); }, 10};
    TrayIcon tray;
    std::optional<GlobalHotKey> hotKey;
};

int main()
{
    eacp::Apps::run<TrayApp>();
    return 0;
}
