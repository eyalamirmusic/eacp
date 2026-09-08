#include "LinuxWindowSystem-Linux.h"

#include "WaylandDisplay-Linux.h"
#include "WaylandInput-Linux.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Utils/Environment.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace eacp::Graphics
{
namespace
{
LinuxWindowSystem linuxWindowSystemNamed(std::string_view name)
{
    if (name == "wayland")
        return LinuxWindowSystem::Wayland;

    if (name == "x11")
        return LinuxWindowSystem::X11;

    return LinuxWindowSystem::None;
}

LinuxWindowSystem linuxChooseWindowSystem()
{
    if (Apps::getAppEnvironment().headless)
        return LinuxWindowSystem::None;

    auto requested = getEnvValue("EACP_WINDOW_SYSTEM");
    std::transform(requested.begin(),
                   requested.end(),
                   requested.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });

    if (auto named = linuxWindowSystemNamed(requested);
        named != LinuxWindowSystem::None)
        return named;

    if (Platform::isDLL())
        return LinuxWindowSystem::X11;

    return waylandCompositorIsReachable() ? LinuxWindowSystem::Wayland
                                          : LinuxWindowSystem::X11;
}

WaylandInput* linuxSeatInput()
{
    if (auto* connection = waylandDisplay())
        return connection->getInput();

    return nullptr;
}
} // namespace

LinuxWindowSystem linuxPreferredWindowSystem()
{
    static const auto preferred = linuxChooseWindowSystem();
    return preferred;
}

std::optional<LinuxOutput> linuxPrimaryOutput()
{
    auto* connection = waylandDisplay();

    if (connection == nullptr)
        return {};

    const auto* output = connection->getPrimaryOutput();

    if (output == nullptr)
        return {};

    auto size = output->logicalSize();

    // Announced, but not yet its mode.
    if (size.x <= 0.f || size.y <= 0.f)
        return {};

    return LinuxOutput {{output->position.x, output->position.y, size.x, size.y},
                        (float) output->scale,
                        output->refreshMilliHz};
}

LinuxWindowSurface* linuxPointerWindow()
{
    if (auto* input = linuxSeatInput())
        return input->getPointerWindow();

    return nullptr;
}

Point linuxPointerPosition()
{
    if (auto* input = linuxSeatInput())
        return input->getPointerPosition();

    return {};
}

void linuxRefreshCursor()
{
    if (auto* input = linuxSeatInput())
        input->refreshCursor();
}

void linuxInstallClipboard(LinuxWindowSystem system, Clipboard::Backend backend)
{
    if (system != linuxPreferredWindowSystem())
        return;

    Clipboard::setBackend(std::move(backend));
}

void linuxClearClipboard(LinuxWindowSystem system)
{
    if (system != linuxPreferredWindowSystem())
        return;

    Clipboard::clearBackend();
}
} // namespace eacp::Graphics
