#include "Api.h"
#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Threads/Timer.h>
#include <eacp/Core/App/Clipboard.h>
#include <eacp/Graphics/Graphics.h>
#include <eacp/Graphics/Graphics/GraphicsContext.h>
#include <eacp/Graphics/Primitives/TextMetrics.h>
#include <eacp/WebView/WebView.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

using namespace eacp;
using namespace Graphics;

static constexpr ModifierKeys launcherModifiers {.alt = true, .command = true};
static constexpr uint16_t launcherKeyCode = KeyCode::L;
static constexpr const char* searchDownloadsCommand = "searchDownloads";
static constexpr auto copyToastDuration = std::chrono::milliseconds {1500};

// The "Copied" HUD, drawn natively rather than through a WebView — a tiny
// confirmation card doesn't justify a second WebView2 environment (which also
// rendered blank while it span up). Fills its rounded window with a dark card
// and centres the asterisk mark, title and detail; the toast timer drives the
// fade via View::setOpacity.
class CopyToastView : public View
{
public:
    void setSampleName(const std::string& name)
    {
        detail = name + " to clipboard";
        repaint();
    }

    void paint(Context& g) override
    {
        auto bounds = getLocalBounds();

        g.setColor(Color {0.110f, 0.110f, 0.125f, 0.96f});
        g.fillRect(bounds);

        auto centreX = bounds.x + bounds.w * 0.5f;

        paintMark(g, centreX, bounds.y + 58.f);

        g.setColor(Color {0.965f, 0.957f, 0.949f, 1.f});
        drawCentred(g, "Copied", centreX, bounds.y + 130.f, titleFont);

        g.setColor(Color {0.965f, 0.957f, 0.949f, 0.66f});
        drawCentred(g, detail, centreX, bounds.y + 160.f, detailFont);
    }

private:
    void drawCentred(Context& g,
                     const std::string& text,
                     float centreX,
                     float baselineY,
                     const Font& font) const
    {
        auto width = TextMetrics::measureWidth(text, font);
        g.drawText(text, {centreX - width * 0.5f, baselineY}, font);
    }

    // Eight rounded bars fanned around a centre — the same asterisk mark the
    // launcher composer shows.
    void paintMark(Context& g, float centreX, float centreY) const
    {
        constexpr auto bars = 8;
        constexpr auto half = 26.f;

        g.setColor(Color {0.945f, 0.945f, 0.949f, 1.f});

        for (auto i = 0; i < bars; ++i)
        {
            g.saveState();
            g.translate(centreX, centreY);
            g.rotate(static_cast<float>(i) * 3.14159265f / bars);
            g.fillRoundedRect(Rect {-3.f, -half, 6.f, half * 2.f}, 3.f);
            g.restoreState();
        }
    }

    std::string detail = " to clipboard";
    Font titleFont {FontOptions().withSize(25.f)};
    Font detailFont {FontOptions().withSize(13.f)};
};

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

        copyToastWindow.setContentView(copyToastView);
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
        copyToastView.setSampleName(sampleName);
        copyToastVisible = true;
        copyToastStart = std::chrono::steady_clock::now();
        copyToastView.setOpacity(0.f);
        copyToastWindow.setVisible(true);
    }

    void updateCopyToast()
    {
        if (!copyToastVisible)
            return;

        auto elapsed = std::chrono::steady_clock::now() - copyToastStart;

        if (elapsed >= copyToastDuration)
        {
            copyToastVisible = false;
            copyToastWindow.setVisible(false);
            return;
        }

        copyToastView.setOpacity(copyToastOpacity(elapsed));
    }

    // A quick fade in, a hold, then a fade out over the toast's lifetime.
    static float copyToastOpacity(std::chrono::steady_clock::duration elapsed)
    {
        using Millis = std::chrono::duration<float, std::milli>;
        constexpr auto fadeIn = 130.f;
        constexpr auto fadeOut = 300.f;

        auto total = std::chrono::duration_cast<Millis>(copyToastDuration).count();
        auto now = std::chrono::duration_cast<Millis>(elapsed).count();
        auto remaining = total - now;

        if (now < fadeIn)
            return now / fadeIn;

        if (remaining < fadeOut)
            return std::clamp(remaining / fadeOut, 0.f, 1.f);

        return 1.f;
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
    CopyToastView copyToastView;
    Window copyToastWindow {getCopyToastOptions()};
    bool copyToastVisible = false;
    std::chrono::steady_clock::time_point copyToastStart {};
    Threads::Timer copyToastTimer {[this] { updateCopyToast(); }, 10};
    TrayIcon tray;
    std::optional<GlobalHotKey> hotKey;
};

int main()
{
    eacp::Apps::run<TrayApp>();
    return 0;
}
