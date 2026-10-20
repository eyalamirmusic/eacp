#include "ViewABI.h"

#include <eacp/Core/Plugins/HotReload.h>
#include <eacp/Core/Plugins/HotReloadLoop.h>

#include <optional>
#include <string>

// A window filled by one GPUView that a factory in Views.cpp beside this file
// creates. The view's type, its shader and its code all live in Views.cpp; the
// app only owns the pointer the factory hands back. The factory is always
// compiled into the app, and under EACP_HOT_RELOAD it is also built as a module,
// HotReloadGPUView, bound at load time to this app's own eacp: swap the view the
// factory returns, or a shader's tuning, in Views.cpp and save, a
// Plugins::HotReloadLoop rebuilds and reloads the module, and the old view is
// destroyed while its image is still mapped and replaced by one the fresh image
// makes.

using namespace eacp;

namespace
{
struct DemoRoot final : Graphics::View
{
    DemoRoot()
    {
        install(&makeDemoView);

        if constexpr (Plugins::isHotReload())
            startReloadLoop();
    }

    void startReloadLoop()
    {
        auto& loop = reloader.emplace(Plugins::hotReloadTarget(), viewFactoryName);

        auto showState = [this] { showStatus(); };
        auto useImage = [this](Plugins::ReloadableLibrary& library)
        { replaceViewFrom(library); };
        auto printFailure = [this](const auto&)
        { Console::print(reloader->describeState()); };

        loop.onStateChanged = showState;
        loop.onReloaded = useImage;
        loop.onBuildFailed = printFailure;
        loop.onLoadFailed = printFailure;
    }

    void resized() override { current->setBounds(getLocalBounds()); }

    void install(ViewFactory factory)
    {
        current = factory();
        addSubview(*current);
        current->setBounds(getLocalBounds());
    }

    void replaceViewFrom(Plugins::ReloadableLibrary& library)
    {
        removeSubview(*current);
        current.reset();

        install(library.findFunction<ViewFactory>(viewFactoryName));
        Console::print("generation " + std::to_string(library.getGeneration()));
    }

    void showStatus()
    {
        onStatusChanged(reloader ? reloader->describeState() : "static view");
    }

    std::function<void(const std::string& status)> onStatusChanged =
        [](const std::string&) {};

    ViewPointer current;
    std::optional<Plugins::HotReloadLoop> reloader;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 960;
    options.height = 600;
    options.title = "eacp GPU — hot-reloaded view";
    options.minWidth = 320;
    options.minHeight = 240;
    return options;
}

struct DemoApp
{
    DemoApp()
    {
        auto showInTitle = [this](const std::string& status)
        { window.setTitle(makeOptions().title + " — " + status); };

        root.onStatusChanged = showInTitle;
        root.showStatus();
    }

    DemoRoot root;
    Graphics::Window window {root, makeOptions()};
};
} // namespace

int main()
{
    return Apps::run<DemoApp>();
}
