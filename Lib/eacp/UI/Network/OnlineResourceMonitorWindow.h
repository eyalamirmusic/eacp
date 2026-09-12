#pragma once

#include "OnlineResourceMonitor.h"

#include <eacp/UI/Host/ComponentHost.h>

namespace eacp::UI
{
// The monitor as the whole of a component tree: a host with it as the root
// and the stock UI face set, for an app that hands a window's content view
// to it and nothing else.
class OnlineResourceMonitorHost final : public ComponentHost
{
public:
    OnlineResourceMonitorHost();

    OnlineResourceMonitor& getMonitor() { return monitor; }

private:
    OnlineResourceMonitor monitor;
};

// The monitor in a window of its own - a downloads panel an app opens and
// forgets. Declare the resources with OnlineResources::get() and construct
// one; the window shows what is on disk, fetches, cancels and clears.
class OnlineResourceMonitorWindow
{
public:
    static eacp::Graphics::WindowOptions defaultOptions();

    explicit OnlineResourceMonitorWindow(
        const eacp::Graphics::WindowOptions& options = defaultOptions());

    OnlineResourceMonitor& getMonitor() { return host.getMonitor(); }
    eacp::Graphics::Window& getWindow() { return window; }

private:
    OnlineResourceMonitorHost host;
    eacp::Graphics::Window window;
};
} // namespace eacp::UI
