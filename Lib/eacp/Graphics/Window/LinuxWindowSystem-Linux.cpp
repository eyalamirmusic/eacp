#include "LinuxWindowSystem-Linux.h"

#include "WaylandDisplay-Linux.h"
#include "WaylandInput-Linux.h"
#include "X11Connection-Linux.h"
#include "X11Input-Linux.h"

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

std::optional<LinuxOutput> waylandPrimaryOutput()
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

// One scale, because X11 has one: a per-window one arrives with Xft.dpi in
// stage 5.
std::optional<LinuxOutput> x11PrimaryOutput()
{
    auto* connection = x11Connection();

    if (connection == nullptr)
        return {};

    auto output = connection->getPrimaryOutput();

    if (!output || output->frame.w <= 0.f || output->frame.h <= 0.f)
        return {};

    return LinuxOutput {output->frame, 1.f, output->refreshMilliHz};
}
} // namespace

LinuxWindowSystem linuxPreferredWindowSystem()
{
    static const auto preferred = linuxChooseWindowSystem();
    return preferred;
}

std::optional<LinuxOutput> linuxPrimaryOutput()
{
    switch (linuxPreferredWindowSystem())
    {
        case LinuxWindowSystem::Wayland:
            return waylandPrimaryOutput();

        case LinuxWindowSystem::X11:
            return x11PrimaryOutput();

        case LinuxWindowSystem::None:
            break;
    }

    return {};
}

LinuxSeat* linuxSeat()
{
    switch (linuxPreferredWindowSystem())
    {
        case LinuxWindowSystem::Wayland:
            if (auto* connection = waylandDisplay())
                return connection->getInput();
            break;

        case LinuxWindowSystem::X11:
            if (auto* connection = x11Connection())
                return connection->getInput();
            break;

        case LinuxWindowSystem::None:
            break;
    }

    return nullptr;
}

LinuxWindowSurface* linuxPointerWindow()
{
    if (auto* seat = linuxSeat())
        return seat->getPointerWindow();

    return nullptr;
}

Point linuxPointerPosition()
{
    if (auto* seat = linuxSeat())
        return seat->getPointerPosition();

    return {};
}

void linuxRefreshCursor()
{
    if (auto* seat = linuxSeat())
        seat->refreshCursor();
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
