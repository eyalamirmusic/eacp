#include <eacp/Core/Plugins/Reloadable.h>
#include <eacp/Graphics/Graphics.h>

#include <string>

// A window filled by one GPUView that a factory in Views.cpp beside this file
// creates. The view's type, its shader and its code all live in Views.cpp; the
// app only owns what the factory hands back. Under EACP_HOT_RELOAD, swap the
// view the factory returns, or a shader's tuning, and save: Plugins::Reloadable
// rebuilds the module, and onReloaded replaces the view with one the fresh
// image makes while the old image is still mapped. See Docs/HotReload.md.

using namespace eacp;

namespace
{
struct DemoRoot final : Graphics::View
{
    DemoRoot() { addChildren({content}); }

    Plugins::ReloadableOptions reloadOptions()
    {
        auto options = Plugins::ReloadableOptions {};

        options.onStateChanged = [&]
        { onStatusChanged("view: " + reloadable.describeState()); };
        options.onReloaded = [&] { content.set(reloadable.create<View>()); };

        return options;
    }

    void resized() override { content.setBounds(getLocalBounds()); }

    std::function<void(const std::string& status)> onStatusChanged =
        [](const std::string&) {};

    Graphics::OptionalView<> content;
    Plugins::Reloadable reloadable {reloadOptions()};
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
    }

    DemoRoot root;
    Graphics::Window window {root, makeOptions()};
};
} // namespace

int main()
{
    return Apps::run<DemoApp>();
}
