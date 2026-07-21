#include <eacp/Graphics/HotKey/GlobalHotKey.h>
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

// The demo is deliberately a REGULAR dock app (setDockIconVisible(true)) — the
// hard case Librarian hit. A Regular app's plain key window is inert unless the
// app is frontmost, and activating it would drop the user out of a full-screen
// DAW's Space. Only a NonactivatingPanel takes the keyboard over full screen
// without activating, which is exactly what this proves.
struct TrayApp
{
    TrayApp()
    {
        Apps::setDockIconVisible(true);

        webView.loadHTML(std::string {panelHtml});
        webView.addScriptMessageHandler(
            "hello",
            [](const std::string& name)
            { LOG("hello ", name.empty() ? std::string("there") : name); });
        webView.addScriptMessageHandler("dismiss",
                                        [this](const std::string&) { hidePanel(); });

        window.setContentView(webView);
        window.setVisible(false);

        tray.setIcon(makeTrayIcon());
        tray.setTooltip("Non-activating panel demo");
        tray.setMenu(createTrayMenu());
        tray.setOnClick([this] { togglePanel(); });

        // Opt+Cmd+L toggles the panel from anywhere, even over a full-screen app.
        hotKey.emplace(ModifierKeys {.alt = true, .command = true},
                       KeyCode::L,
                       [this] { togglePanel(); });
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
        menu.add(MenuItem::withAction("Toggle Panel (Opt+Cmd+L)",
                                      [this] { togglePanel(); }));
        menu.addSeparator();
        menu.add(MenuItem::withAction("Quit", [] { Apps::quit(); }));
        return menu;
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
    Window window {getPanelOptions()};
    TrayIcon tray;
    std::optional<GlobalHotKey> hotKey;
};

int main()
{
    return eacp::Apps::run<TrayApp>();
}
