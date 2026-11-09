#include "../X11PluginABI.h"

#include <eacp/Core/App/Args.h>
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

FilePath pluginPath {X11_PLUGIN_PATH};
Time::MS exitAfter;

// nullopt to go on, else the exit code: --help, or arguments that did not parse.
std::optional<int> readArguments(int argc, char** argv)
{
    auto cli = Args::Parser {"X11Host", "Embeds an X11 plugin GUI as a DAW would."};

    cli.option("--exit-after-ms",
               exitAfter.count,
               "quit by itself after this long, resizing the window halfway")
        .atLeast(0)
        .hint("<ms>");
    cli.positional(pluginPath, "plugin", "the plugin library to load")
        .defaultText("the bundled one");

    return cli.exitCodeFor(cli.parse(argc, argv));
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

    if (auto code = readArguments(argc, argv))
        return *code;

    return Apps::run<HostApp>();
}
