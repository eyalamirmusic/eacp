#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

// The four WebView options a browser-shaped app turns on, all on at once.
// The page reports what the web view tells it: the user agent it sends and
// whether the Fullscreen API is allowed. Swipe two fingers back after
// following the link, and pinch the page, to see the other two.

constexpr auto safariUserAgentTail = "Version/26.0 Safari/605.1.15";

std::string demoPage()
{
    return R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><style>
  body { font: 15px/1.6 -apple-system, "Segoe UI", sans-serif; margin: 0;
         padding: 30px; background: #14161a; color: #e6e8ec; }
  h2 { font-size: 12px; text-transform: uppercase; letter-spacing: .1em;
       color: #8b93a1; margin: 24px 0 8px; }
  pre { background: #1d2027; border: 1px solid #2b303a; border-radius: 8px;
        padding: 14px; margin: 0; white-space: pre-wrap; word-break: break-all; }
  a, button { color: #6ea8fe; font: inherit; }
  #stage { background: #2b5fb4; color: white; padding: 20px; border-radius: 8px; }
</style></head>
<body>
  <h2>applicationNameForUserAgent: navigator.userAgent ends with Safari's tail</h2>
  <pre id="ua"></pre>
  <h2>elementFullscreen: document.fullscreenEnabled</h2>
  <pre id="fullscreen"></pre>
  <div id="stage"><button id="enter">Make this box fullscreen</button></div>
  <h2>backForwardGestures</h2>
  <p><a href="https://example.com">Go to example.com</a>, then swipe two fingers
     right on the trackpad to come back here.</p>
  <h2>allowsMagnification</h2>
  <p>Pinch the trackpad to zoom this page.</p>
  <script>
    document.getElementById("ua").textContent = navigator.userAgent;
    document.getElementById("fullscreen").textContent =
        String(document.fullscreenEnabled);
    document.getElementById("enter").onclick =
        () => document.getElementById("stage").requestFullscreen();
  </script>
</body></html>)HTML";
}

WebView::Options getWebViewOptions()
{
    auto options = WebView::Options();
    options.applicationNameForUserAgent = safariUserAgentTail;
    options.backForwardGestures = true;
    options.allowsMagnification = true;
    options.elementFullscreen = true;
    return options;
}

struct BrowserOptionsView final : View
{
    BrowserOptionsView()
    {
        webView.loadHTML(demoPage());
        addChildren({webView});
    }

    void resized() override { webView.setBounds(getLocalBounds()); }

    WebView webView {getWebViewOptions()};
};

WindowOptions getWindowOptions()
{
    auto options = WindowOptions();
    options.title = "WebView Browser Options";
    options.width = 900;
    options.height = 680;
    return options;
}

int main()
{
    return Graphics::runWindowedApp<BrowserOptionsView>(getWindowOptions());
}
