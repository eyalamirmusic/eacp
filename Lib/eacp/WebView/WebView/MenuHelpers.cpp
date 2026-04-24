#include "MenuHelpers.h"

namespace eacp::Graphics
{

void WebHelpers::zoomInFocusedWebView()
{
    if (auto w = WebView::focused())
        w->zoomIn();
}

void WebHelpers::zoomOutFocusedWebView()
{
    if (auto w = WebView::focused())
        w->zoomOut();
}

void WebHelpers::resetSizedFocusedWebView()
{
    if (auto w = WebView::focused())
        w->resetZoom();
}
MenuBar buildDefaultWebViewMenuBar()
{
    using namespace WebHelpers;

    auto bar = MenuBar {};
    bar.add(standardApplicationMenu("WebView Embed"));

    auto view = Menu {"View"};
    view.add(MenuItem::withAction("Zoom In", zoomInFocusedWebView, commandKey("+")));
    view.add(
        MenuItem::withAction("Zoom Out", zoomOutFocusedWebView, commandKey("-")));
    view.add(MenuItem::withAction(
        "Actual Size", resetSizedFocusedWebView, commandKey("0")));

    bar.add(std::move(view));

    return bar;
}
} // namespace eacp::Graphics
