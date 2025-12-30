#pragma once

#include <string>
#include <memory>
#include <vector>
#include "View.h"

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

    int width = 640;
    int height = 400;
    std::string title = "New Window";

    std::vector<WindowFlags> flags;
};

class Window
{
public:
    Window(const WindowOptions& options = {});

    void setTitle(const std::string& title);
    void close();
    void* getHandle();

    void setContentView(View& view);

private:

    struct Impl;
    std::shared_ptr<Impl> impl;
};

} // namespace eacp::Graphics