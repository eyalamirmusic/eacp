#include "../SpinningTriangle.h"

#include <eacp/Graphics/Graphics.h>

#include <utility>

using namespace eacp;

namespace
{
struct App
{
    App()
    {
        window.setTitle("PluginHost (host eacp)");

        if (!library.isOpen())
        {
            LOG("Host: failed to load ", DEMO_PLUGIN_PATH);
            Apps::quit();
            return;
        }

        if (auto getName = library.findFunction<const char* (*) ()>("demo_get_name"))
            LOG("Host: loaded '", getName(), "'");

        if (auto openWindow = library.findFunction<void (*)()>("demo_open_window"))
            openWindow();

        if (auto tryQuit = library.findFunction<void (*)()>("demo_try_quit"))
            tryQuit();
    }

    void update()
    {
        auto closeWindow = library.findFunction<void (*)()>("demo_close_window");

        // The documented teardown: the plugin destroys its window first, and
        // the image is unmapped a loop turn later, once anything the close
        // queued has run. The quit rides the same queue behind it.
        Plugins::unload(std::move(library),
                        [closeWindow]
                        {
                            if (closeWindow != nullptr)
                                closeWindow();
                        });

        Threads::callAsync(
            [this]
            {
                LOG("Host: plugin unloaded, quitting (host frames drawn: ",
                    view.framesRendered,
                    ")");
                Apps::quit();
            });
    }

    // Blue to the plugin's orange, and standing still: the host copy has
    // nothing animating it.
    PluginDemo::SpinningTriangleView view {{0.2f, 0.4f, 0.8f}};
    Plugins::DynamicLibrary library {DEMO_PLUGIN_PATH};
    Graphics::Window window {view};
    Threads::Timer timer {[&] { update(); }, 1};
};
} // namespace

int main()
{
    return Apps::run<App>();
}
