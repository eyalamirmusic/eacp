#include <eacp/Graphics/Graphics.h>

struct HostView final : eacp::Graphics::View
{
    HostView()
    {
        layer->setFillColor({0.2f, 0.4f, 0.8f});
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

struct App
{
    App()
    {
        window.setTitle("PluginHost (host eacp)");
        window.setContentView(view);

        if (!library.isOpen())
        {
            eacp::LOG("Host: failed to load ", DEMO_PLUGIN_PATH);
            eacp::Apps::quit();
            return;
        }

        if (auto getName = library.findFunction<const char* (*) ()>("demo_get_name"))
            eacp::LOG("Host: loaded '", getName(), "'");

        if (auto openWindow = library.findFunction<void (*)()>("demo_open_window"))
            openWindow();

        if (auto tryQuit = library.findFunction<void (*)()>("demo_try_quit"))
            tryQuit();
    }

    void update()
    {
        if (auto closeWindow = library.findFunction<void (*)()>("demo_close_window"))
            closeWindow();

        library.close();
        eacp::LOG("Host: plugin unloaded, quitting");
        eacp::Apps::quit();
    }

    eacp::Plugins::DynamicLibrary library {DEMO_PLUGIN_PATH};
    HostView view;
    eacp::Graphics::Window window;
    eacp::Threads::Timer timer {[&] { update(); }, 1};
};

int main()
{
    return eacp::Apps::run<App>();
}
