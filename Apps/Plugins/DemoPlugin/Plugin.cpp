#include "../SpinningTriangle.h"

#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Graphics/Graphics.h>

#include <memory>

namespace
{
struct PluginWindow
{
    PluginWindow() { window.setTitle("DemoPlugin (plugin's own eacp)"); }

    void tick()
    {
        if (++ticks == 1)
            eacp::LOG("DemoPlugin: timer tick via the plugin's own eacp copy");

        view.advance();
    }

    // Orange to the host's blue, and turning for as long as the plugin copy's
    // own loop is being served.
    eacp::PluginDemo::SpinningTriangleView view {{0.9f, 0.4f, 0.1f}};
    eacp::Graphics::Window window {view};
    int ticks = 0;
    eacp::Threads::Timer timer {[&] { tick(); }, 10};
};

std::unique_ptr<PluginWindow> pluginWindow;
} // namespace

EACP_PLUGIN_EXPORT const char* demo_get_name()
{
    return "Demo Plugin 1.0";
}

EACP_PLUGIN_EXPORT void demo_open_window()
{
    pluginWindow = std::make_unique<PluginWindow>();
    eacp::LOG("DemoPlugin: window created by the plugin's own eacp copy");

    eacp::Threads::callAsync(
        []
        {
            eacp::LOG("DemoPlugin: callAsync delivered through the plugin's "
                      "own eacp copy");
        });
}

EACP_PLUGIN_EXPORT void demo_try_quit()
{
    eacp::LOG("DemoPlugin: calling Apps::quit() from the hosted copy "
              "(must not stop the host)");
    eacp::Apps::quit();
}

EACP_PLUGIN_EXPORT void demo_close_window()
{
    if (pluginWindow != nullptr)
        eacp::LOG("DemoPlugin: window closed (frames drawn: ",
                  pluginWindow->view.framesRendered,
                  ")");

    pluginWindow.reset();
}
