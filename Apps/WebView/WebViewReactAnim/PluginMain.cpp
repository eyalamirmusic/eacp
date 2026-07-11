#include "Types.h"

#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Threads/Timer.h>
#include <eacp/WebView/WebView.h>

#include <memory>

// The same app as Main.cpp, packaged as a runtime-loaded plugin: the host
// owns the event loop, so the window is created and destroyed through the
// exported entry points instead of Apps::run<MyApp>().
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
