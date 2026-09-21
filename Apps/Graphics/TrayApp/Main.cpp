#include <eacp/Graphics/HotKey/GlobalHotKey.h>
#include <eacp/UI/UI.h>
#include <eacp/WebView/WebView.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

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

// A self-contained page — an auto-focusing text box that posts the typed name
// to native on Enter and asks to be dismissed on Esc. Mirrors the Librarian
// mini-panel input: the point of the demo is that a WKWebView input inside a
// non-activating panel is typeable over another app's full-screen Space
// without the owning app ever activating.
static constexpr std::string_view panelHtml = R"html(
<!doctype html>
<meta charset="utf-8">
<style>
  html, body { margin: 0; height: 100%; }
  body {
    background: #17171a; color: #ececf0;
    font: 16px -apple-system, system-ui, sans-serif;
  }
  .wrap {
    box-sizing: border-box; height: 100%;
    padding: 20px; display: flex; flex-direction: column; gap: 12px;
  }
  h1 {
    margin: 0; font-size: 12px; font-weight: 600;
    letter-spacing: .04em; color: #8a8a94; text-transform: uppercase;
  }
  input {
    width: 100%; box-sizing: border-box; padding: 12px 14px;
    border-radius: 10px; border: 1px solid #33333c; background: #0e0e11;
    color: #fff; font-size: 18px; outline: none;
  }
  input:focus { border-color: #4ade80; }
  p { margin: 0; font-size: 12px; color: #6a6a74; }
</style>
<div class="wrap">
  <h1>Non-activating panel &middot; &#8997;&#8984;L</h1>
  <input id="name" name="search" placeholder="Type your name, hit &#9166;&#8230;"
         autocomplete="off" spellcheck="false">
  <p>&#9166; prints &ldquo;hello &lt;name&gt;&rdquo; to the terminal &middot; esc hides</p>
</div>
<script>
  const input = document.getElementById('name');
  const focus = () => { input.focus(); input.select(); };
  focus();
  setTimeout(focus, 0); setTimeout(focus, 50); setTimeout(focus, 150);
  window.addEventListener('focus', focus);
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      window.webkit.messageHandlers.hello.postMessage(input.value);
      input.select();
    } else if (e.key === 'Escape') {
      window.webkit.messageHandlers.dismiss.postMessage('');
    }
  });
</script>
)html";

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

// The demo is a REGULAR dock app while its main window is up (showMainWindow
// below turns the Dock icon on) — the
// hard case Librarian hit. A Regular app's plain key window is inert unless the
// app is frontmost, and activating it would drop the user out of a full-screen
// DAW's Space. Only a NonactivatingPanel takes the keyboard over full screen
// without activating, which is exactly what this proves.
struct TrayApp
{
    TrayApp()
    {
        webView.loadHTML(std::string {panelHtml});
        webView.addScriptMessageHandler(
            "hello",
            [](const std::string& name)
            { LOG("hello ", name.empty() ? std::string("there") : name); });
        webView.addScriptMessageHandler("dismiss",
                                        [this](const std::string&) { hidePanel(); });

        window.setContentView(webView);

        // The panel shows itself on construction; hide it immediately so the
        // app starts with the main window only. setVisible keeps the window
        // (and its content) alive across toggles, so it reappears exactly
        // where the user left it.
        window.setVisible(false);

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
        tray.setTooltip("Non-activating panel demo");
        tray.setMenu(createTrayMenu());
        tray.setOnClick([this] { togglePanel(); });

        // The bundle is LSUIElement, so the app launched with no Dock icon
        // and the main window is already on screen — promote it to a regular
        // Dock app to match, with no icon flash on the way.
        showMainWindow(true);

        // Opt+Cmd+L toggles the panel from anywhere, even over a full-screen app.
        hotKey.emplace(ModifierKeys {.alt = true, .command = true},
                       KeyCode::L,
                       [this] { togglePanel(); });
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

    // Borderless + rounded, floating above normal windows and following the
    // user across Spaces (including onto another app's full-screen Space), and
    // — the whole point — a non-activating panel so it can be keyed without the
    // app activating. showInactive so construction never steals focus.
    static WindowOptions getPanelOptions()
    {
        auto options = WindowOptions();

        options.width = 420;
        options.height = 172;
        options.isPrimary = false;

        options.flags = {WindowFlags::Borderless, WindowFlags::NonactivatingPanel};
        options.cornerRadius = 16.f;

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
        menu.add(MenuItem::withAction("Toggle Panel (Opt+Cmd+L)",
                                      [this] { togglePanel(); }));
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

    void togglePanel()
    {
        if (window.isVisible())
            hidePanel();
        else
            reveal();
    }

    void reveal()
    {
        window.focusWithoutActivating();
        webView.focusContent();
    }

    void hidePanel() { window.setVisible(false); }

    WebView webView;
    MainHost mainHost;
    Window window {getPanelOptions()};
    Window mainWindow {mainHost, getMainWindowOptions()};
    TrayIcon tray;
    std::optional<GlobalHotKey> hotKey;
};

int main()
{
    return eacp::Apps::run<TrayApp>();
}
