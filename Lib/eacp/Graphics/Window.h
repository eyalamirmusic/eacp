#pragma once

#include <string>
#include <memory>
#include <vector>
#include "View.h"
#include "../App/App.h"

namespace eacp::Graphics
{

enum class WindowFlags
{
    Borderless,
    Titled,
    Closable,
    Miniaturizable,
    Resizable,
    UnifiedTitleAndToolbar,
    FullScreen,
    FullSizeContentView,
    UtilityWindow,
    DocModalWindow,
    NonactivatingPanel,
    HUDWindow
};

struct WindowOptions
{
    WindowOptions()
    {
        flags.emplace_back(WindowFlags::Titled);
        flags.emplace_back(WindowFlags::Closable);
        flags.emplace_back(WindowFlags::Miniaturizable);
        flags.emplace_back(WindowFlags::Resizable);
    }

    Callback onQuit {Apps::quit};

    int width = 640;
    int height = 400;
    std::string title = "New Window";

    std::vector<WindowFlags> flags;
};

class Window
{
public:
    Window(const WindowOptions& optionsToUse = {});
    ~Window();

    void setTitle(const std::string& title);
    void* getHandle();

    void setContentView(View& view);

private:
    WindowOptions options;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics