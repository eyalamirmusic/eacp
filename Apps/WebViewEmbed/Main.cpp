#include <eacp/WebView/WebView.h>
#include <WebResources.h>

using namespace eacp;
using namespace Graphics;

namespace
{
void runOnFocusedWebView(void (WebView::*method)())
{
    if (auto* wv = WebView::focused())
        (wv->*method)();
}

MenuBar buildMenuBar()
{
    auto bar = MenuBar {};
    bar.add(standardApplicationMenu("WebView Embed"));

    auto view = Menu {"View"};
    view.add(MenuItem::withAction(
        "Zoom In", [] { runOnFocusedWebView(&WebView::zoomIn); }, commandKey("+")));
    view.add(MenuItem::withAction(
        "Zoom Out",
        [] { runOnFocusedWebView(&WebView::zoomOut); },
        commandKey("-")));
    view.add(MenuItem::withAction(
        "Actual Size",
        [] { runOnFocusedWebView(&WebView::resetZoom); },
        commandKey("0")));
    bar.add(std::move(view));

    return bar;
}
} // namespace

struct MyApp
{
    MyApp()
    {
        setApplicationMenuBar(buildMenuBar());
        window.setContentView(webView);
    }

    WebView webView {embeddedOptions("WebApp")};
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}
