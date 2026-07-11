#include <eacp/Core/Core.h>
#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Graphics/Graphics.h>

#include <memory>

namespace
{
struct PluginView final : eacp::Graphics::View
{
    PluginView()
    {
        layer->setFillColor({0.9f, 0.4f, 0.1f});
        addChildren({layer});
    }

    void resized() override
    {
        auto path = eacp::Graphics::Path();
        path.addRoundedRect(getLocalBounds(), 12.f);
        layer->setPath(path);
        scaleToFit({layer});
    }

    eacp::Graphics::ShapeLayerView layer;
};

struct PluginWindow
{
    PluginWindow()
    {
        window.setTitle("DemoPlugin (plugin's own eacp)");
        window.setContentView(view);
    }

    void tick()
    {
        if (++ticks == 1)
            eacp::LOG("DemoPlugin: timer tick via the plugin's own eacp copy");
    }

    PluginView view;
    eacp::Graphics::Window window;
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
    pluginWindow.reset();
    eacp::LOG("DemoPlugin: window closed");
}
