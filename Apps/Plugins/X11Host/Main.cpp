#include "../X11PluginABI.h"

#include <eacp/Core/Threads/EventLoop-Linux.h>
#include <eacp/Graphics/Graphics.h>

#include <cstdlib>
#include <poll.h>
#include <string>
#include <string_view>

using namespace eacp;

namespace
{
// What a DAW does with a plugin's GUI: hand over a window id, register the
// descriptor the plugin asks for, pump it whenever it is readable, and on the
// way out take the descriptor back before letting the image go.
class HostedPlugin
{
public:
    explicit HostedPlugin(const FilePath& path)
        : library(path)
    {
    }

    ~HostedPlugin() { detach(); }

    bool attach(unsigned long parentWindowId, double scale)
    {
        if (!library.isOpen())
        {
            LOG("X11Host: could not load the plugin");
            return false;
        }

        auto open =
            library.findFunction<X11PluginABI::Open>(X11PluginABI::openSymbol);
        auto loopFd =
            library.findFunction<X11PluginABI::LoopFd>(X11PluginABI::loopFdSymbol);
        auto pumpFn =
            library.findFunction<X11PluginABI::Pump>(X11PluginABI::pumpSymbol);

        if (open == nullptr || loopFd == nullptr || pumpFn == nullptr)
        {
            LOG("X11Host: the plugin is missing one of the four entry points");
            return false;
        }

        pump = pumpFn;

        if (open(parentWindowId, scale) == 0)
        {
            LOG("X11Host: the plugin refused window ", parentWindowId);
            return false;
        }

        registeredFd = loopFd();

        if (registeredFd >= 0)
            Threads::addLoopSource(registeredFd, POLLIN, [this] { pump(); });

        LOG("X11Host: plugin attached to window ",
            parentWindowId,
            " at scale ",
            scale,
            ", pumping fd ",
            registeredFd);

        return true;
    }

    void detach()
    {
        if (!library.isOpen())
            return;

        if (registeredFd >= 0)
        {
            Threads::removeLoopSource(registeredFd);
            registeredFd = -1;
        }

        auto close =
            library.findFunction<X11PluginABI::Close>(X11PluginABI::closeSymbol);

        pump = [] {};

        Plugins::unload(std::move(library),
                        [close]
                        {
                            if (close != nullptr)
                                close();
                        });

        LOG("X11Host: plugin closed and unloaded");
    }

private:
    Plugins::DynamicLibrary library;
    X11PluginABI::Pump pump = [] {};
    int registeredFd = -1;
};

FilePath pluginPath;
Time::MS exitAfter;

void readArguments(int argc, char** argv)
{
    constexpr auto exitFlag = std::string_view {"--exit-after-ms="};

    pluginPath = FilePath {X11_PLUGIN_PATH};

    for (auto i = 1; i < argc; ++i)
    {
        auto argument = std::string_view {argv[i]};

        if (argument.starts_with(exitFlag))
            exitAfter = Time::MS {std::atoll(argv[i] + exitFlag.size())};
        else if (!argument.starts_with("-"))
            pluginPath = FilePath {std::string {argument}};
    }
}

Graphics::WindowOptions hostWindowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.title = "X11Host (host eacp)";

    return options;
}

struct HostApp
{
    HostApp()
    {
        window.setVisible(true);

        auto* handle = window.getHandle();

        if (handle == nullptr)
        {
            LOG("X11Host: no X11 window to embed into");
            Apps::quit(1);
            return;
        }

        auto parentWindowId = (unsigned long) reinterpret_cast<uintptr_t>(handle);

        if (!plugin.attach(parentWindowId, Graphics::primaryDisplay().backingScale))
        {
            Apps::quit(1);
            return;
        }

        if (exitAfter > Time::MS {0})
            runUnattended();
    }

    // Nobody is here to drag the window, so the run that has to end by itself
    // resizes it once as well: the surface has never been given bounds of its
    // own, so the plugin's content should follow.
    void runUnattended()
    {
        Threads::callAfter(Time::MS {exitAfter.count / 2},
                           [this] { window.setSize({480.f, 300.f}); });

        Threads::callAfter(exitAfter, [] { Apps::quit(); });
    }

    Graphics::Window window {hostWindowOptions()};
    HostedPlugin plugin {pluginPath};
};
} // namespace

// The preference is read once per copy the first time a window is made, so the
// override has to be in place before anything opens one - in this process both
// copies then take X11, which is the only thing a plugin API can name.
int main(int argc, char** argv)
{
    setenv("EACP_WINDOW_SYSTEM", "x11", 1);

    readArguments(argc, argv);

    return Apps::run<HostApp>();
}
