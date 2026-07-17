#include "TerminalView.h"

#include <eacp/Core/App/App.h>

using namespace eacp;

namespace
{
Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 980;
    options.height = 640;
    options.minWidth = 300;
    options.minHeight = 200;
    options.title = "eacp Terminal";
    options.backgroundColor = term::toColor(term::Theme {}.background);
    return options;
}
} // namespace

struct TerminalApp
{
    TerminalApp()
    {
        view.onTitleChanged = [this](const std::string& title)
        { window.setTitle(title.empty() ? "eacp Terminal" : title); };

        view.onShellExit = [] { Apps::quit(); };

        window.setContentView(view);
        view.focus();
    }

    term::TerminalView view;
    Graphics::Window window {windowOptions()};
};

int main()
{
    return Apps::run<TerminalApp>();
}
