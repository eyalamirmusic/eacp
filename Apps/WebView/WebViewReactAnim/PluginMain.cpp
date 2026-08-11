#include "Types.h"

#include <eacp/WebView/WebView.h>

// Main.cpp's app as a runtime-loaded plugin: the host owns the event loop, so
// the window lives and dies with the exported entry points.
namespace
{
using namespace eacp;
using namespace Graphics;

struct PluginApp
{
    PluginApp()
    {
        transport.getBridge().use(clock);
        window.setTitle("WebViewReactAnim (plugin)");
        window.setContentView(webView);
    }

    Api::Clock clock;
    WebView webView {embeddedOptions("ReactAnimApp")};
    WebViewBridge transport {webView};
    Window window;
    Threads::Timer timer {[&] { clock.update(); }, 120};
};

std::unique_ptr<PluginApp> app;
} // namespace

EACP_PLUGIN_EXPORT void reactanim_open_window()
{
    app = std::make_unique<PluginApp>();
}

EACP_PLUGIN_EXPORT void reactanim_close_window()
{
    app.reset();
}
