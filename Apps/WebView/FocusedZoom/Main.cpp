#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

// Two WebViews in one window, a toolbar page over a content page, the way a
// browser lays them out. View > Zoom In and Zoom Out act on WebView::focused:
// click into either page, zoom, and only that page grows.

constexpr auto toolbarHeight = 170.f;

std::string describePage(const std::string& name, const std::string& background)
{
    return R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><style>
  body { font: 15px/1.6 -apple-system, "Segoe UI", sans-serif; margin: 0;
         padding: 20px; color: #e6e8ec; background: )HTML"
           + background + R"HTML(; }
  input { font: inherit; width: 60%; }
</style></head>
<body>
  <b>)HTML" + name
           + R"HTML(</b>
  <p>Click the box below, then View &gt; Zoom In (Command/Control and +).
     Only the page holding keyboard focus zooms.</p>
  <input placeholder="Type here to focus this page">
</body></html>)HTML";
}

struct FocusedZoomView final : View
{
    FocusedZoomView()
    {
        toolbar.loadHTML(describePage("Toolbar page", "#2b303a"));
        content.loadHTML(describePage("Content page", "#14161a"));
        addChildren({toolbar, content});
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        toolbar.setBounds(bounds.removeFromTop(toolbarHeight));
        content.setBounds(bounds);
    }

    WebView toolbar;
    WebView content;
};

struct FocusedZoomApp
{
    FocusedZoomApp() { setApplicationMenuBar(buildDefaultWebViewMenuBar(), window); }

    static WindowOptions getOptions()
    {
        auto options = WindowOptions();
        options.title = "WebView Focused Zoom";
        options.width = 900;
        options.height = 600;
        return options;
    }

    FocusedZoomView root;
    Window window {root, getOptions()};
};

int main()
{
    return eacp::Apps::run<FocusedZoomApp>();
}
