#pragma once

#include "../../Core/App/App.h"
#include "Window.h"

namespace eacp::Graphics
{
// A view and the window showing it, owned together in the order the pair
// needs: the view is built first, from `args`, and the window is built with it
// as its content. `ViewWindow<MyView> shown {options, viewArgs...};` is the
// whole of what a struct holding one view in one window used to spell out.
template <typename ViewType>
struct ViewWindow
{
    template <typename... Args>
    explicit ViewWindow(const WindowOptions& options = {}, Args&&... args)
        : view(std::forward<Args>(args)...)
        , window(view, options)
    {
    }

    ViewType view;
    Window window;
};

// main() for an app that is one view in one window and nothing else:
// `return Graphics::runWindowedApp<MyView>(options, viewArgs...);` runs
// Apps::run over a ViewWindow<MyView>. The argc/argv form captures the command
// line first, as Apps::run's does.
template <typename ViewType, typename... Args>
int runWindowedApp(const WindowOptions& options = {}, Args&&... args)
{
    return Apps::run<ViewWindow<ViewType>>(options, std::forward<Args>(args)...);
}

template <typename ViewType, typename... Args>
int runWindowedApp(int argc,
                   char* argv[],
                   const WindowOptions& options = {},
                   Args&&... args)
{
    Apps::setCommandLineArgs(argc, argv);
    return runWindowedApp<ViewType>(options, std::forward<Args>(args)...);
}
} // namespace eacp::Graphics
