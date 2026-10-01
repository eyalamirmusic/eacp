#include "AppShell.h"
#include "DaemonClient.h"

#include <eacp/Core/App/App.h>

using namespace eacp;

namespace
{
Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1100;
    options.height = 700;
    options.minWidth = 300;
    options.minHeight = 200;
    options.title = "CowTerm";
    options.backgroundColor =
        term::toColor(term::themeByName(term::loadConfig().theme).background);

    // Closing the window backgrounds the terminal (the tray keeps it
    // reachable); shells and sessions stay alive until an explicit quit.
    options.hidesOnClose = true;
    return options;
}
} // namespace

struct TerminalApp
{
    TerminalApp()
    {
        shell.onWindowTitleChanged = [this](const std::string& title)
        { window.setTitle(title.empty() ? "CowTerm" : title); };

        shell.onBringToFront = [this]
        {
            window.setVisible(true);
            window.toFront();
        };

        Apps::setReopenHandler(
            [this]
            {
                window.setVisible(true);
                window.toFront();
            });

        window.events.onActivationChanged = [this](bool isKey)
        { shell.setWindowFocused(isKey); };

        window.setContentView(shell);

        // Sessions restore once the daemon dial resolves, so panes with
        // still-running shells re-adopt them instead of spawning fresh.
        term::DaemonClient::initialize([this] { shell.start(); });
    }

    term::AppShell shell;
    Graphics::Window window {windowOptions()};
};

int main()
{
    term::registerEmbeddedFonts();
    return Apps::run<TerminalApp>();
}
