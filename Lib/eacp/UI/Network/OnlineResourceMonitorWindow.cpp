#include "OnlineResourceMonitorWindow.h"

namespace eacp::UI
{
OnlineResourceMonitorHost::OnlineResourceMonitorHost()
{
    setFontPointSize(13.f);
    setRootComponent(monitor);
}

eacp::Graphics::WindowOptions OnlineResourceMonitorWindow::defaultOptions()
{
    auto options = eacp::Graphics::WindowOptions {};
    options.width = 820;
    options.height = 560;
    options.minWidth = 520;
    options.minHeight = 320;
    options.title = "Online resources";
    return options;
}

OnlineResourceMonitorWindow::OnlineResourceMonitorWindow(
    const eacp::Graphics::WindowOptions& options)
    : window(host, options)
{
}

OnlineResourceMonitor& OnlineResourceMonitorHost::getMonitor()
{
    return monitor;
}

OnlineResourceMonitor& OnlineResourceMonitorWindow::getMonitor()
{
    return host.getMonitor();
}

eacp::Graphics::Window& OnlineResourceMonitorWindow::getWindow()
{
    return window;
}
} // namespace eacp::UI
