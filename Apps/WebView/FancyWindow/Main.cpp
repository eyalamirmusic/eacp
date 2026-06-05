#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

// Self-contained page demonstrating window dragging. The title bar is marked
// `--eacp-app-region: drag` (the marker eacp's injected window-drag.js reads;
// `-webkit-app-region` is kept alongside for Electron parity), so pressing and
// dragging it moves this frameless window. The button is `no-drag`, proving
// controls stay clickable and don't move the window.
static const char* kDemoHtml = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  html, body { margin: 0; height: 100%;
               font-family: -apple-system, system-ui, sans-serif; }
  body { background: #0b0b0d; color: #fff; display: flex; flex-direction: column; }

  .titlebar {
    -webkit-app-region: drag;
    --eacp-app-region: drag;
    height: 52px;
    display: flex;
    align-items: center;
    gap: 12px;
    padding-left: 88px;   /* clear the traffic lights */
    padding-right: 16px;
    background: linear-gradient(90deg, #1b1b22, #101014);
    border-bottom: 1px solid #26262e;
    user-select: none;
  }
  .titlebar .title { font-weight: 600; font-size: 13px; opacity: .9; }
  .spacer { flex: 1; }

  button {
    -webkit-app-region: no-drag;
    --eacp-app-region: no-drag;
    border: 1px solid #34343f; background: #1e1e26; color: #fff;
    height: 30px; padding: 0 14px; border-radius: 8px;
    font-size: 12px; cursor: pointer;
  }
  button:hover { background: #2a2a34; }

  .content { flex: 1; display: grid; place-items: center;
             text-align: center; padding: 24px; }
  .content h1 { font-size: 20px; margin: 0 0 8px; }
  .content p { margin: 4px 0; color: #b9b9c4; font-size: 13px; max-width: 460px; }
  code { color: #7e89ff; }
  .badge { margin-top: 16px; font-size: 12px; color: #3df1a6; }
</style>
</head>
<body>
  <div class="titlebar">
    <span class="title">&#x283F; Drag this bar to move the window</span>
    <span class="spacer"></span>
    <button id="ping">Click me (no-drag)</button>
  </div>
  <div class="content">
    <div>
      <h1>eacp window-drag demo</h1>
      <p>The bar above is <code>--eacp-app-region: drag</code> &mdash; press and
         drag it to move this frameless window.</p>
      <p>The button is <code>no-drag</code>, so it stays clickable and never
         moves the window.</p>
      <div class="badge" id="status">ready</div>
    </div>
  </div>
  <script>
    var n = 0;
    document.getElementById('ping').addEventListener('click', function () {
      n += 1;
      document.getElementById('status').textContent =
        'button clicked ' + n + 'x — still clickable, not dragging';
    });
  </script>
</body>
</html>
)HTML";

struct RootView final : View
{
    RootView() { addChildren({webView}); }

    void resized() override { scaleToFit({webView}); }

    WebView webView;
};

struct MyApp
{
    MyApp()
    {
        rootView.webView.loadHTML(kDemoHtml);
        window.setContentView(rootView);
    }

    // Mirrors the Electron window's titleBarStyle: 'hidden' + backgroundColor
    // + trafficLightPosition so the native window's chrome matches the web
    // app. A FullSizeContentView with a transparent, separator-less titlebar
    // lets the web app's own header render under the traffic lights as one
    // seamless black bar.
    static WindowOptions getOptions()
    {
        auto options = WindowOptions();

        options.width = 1200;
        options.height = 800;
        options.minWidth = 1200;
        options.minHeight = 800;

        options.flags.emplace_back(WindowFlags::FullSizeContentView);
        options.showTitle = false;

        options.titlebarTransparent = true;
        options.showTitlebarSeparator = false;
        options.trafficLightPosition = Point {10.f, 11.f};
        options.backgroundColor = Color::black();

        return options;
    }

    RootView rootView;
    Window window {getOptions()};
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
