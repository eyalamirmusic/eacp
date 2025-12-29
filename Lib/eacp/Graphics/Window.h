#pragma once

#include <string>
#include <memory>

namespace eacp::Graphics
{

struct WindowOptions
{
    int width = 640;
    int height = 400;
    std::string title = "New Window";
    bool resizable = true;
};

class Window
{
public:
    Window(const WindowOptions& options = {});

    void setTitle(const std::string& title);
    void close();

private:
    // Forward declaration of the internal Obj-C struct
    struct Impl;
    std::shared_ptr<Impl> impl;
};

} // namespace eacp::Graphics