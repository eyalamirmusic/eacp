#include <eacp/Core/Utils/WinInclude.h>

#include "Display.h"

namespace eacp::Graphics
{
namespace
{
// Win32 reports monitor rects in physical pixels; an eacp screen point is a
// 96-DPI unit. GetDpiForSystem, not GetDpiForWindow: there is no window yet.
float systemScale()
{
    const auto dpi = GetDpiForSystem();

    return dpi > 0 ? static_cast<float>(dpi) / 96.f : 1.f;
}

Rect toScreenPoints(const RECT& rect, float scale)
{
    return {static_cast<float>(rect.left) / scale,
            static_cast<float>(rect.top) / scale,
            static_cast<float>(rect.right - rect.left) / scale,
            static_cast<float>(rect.bottom - rect.top) / scale};
}
} // namespace

Display primaryDisplay()
{
    const auto scale = systemScale();

    auto origin = POINT {0, 0};
    auto monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);

    auto info = MONITORINFO {};
    info.cbSize = sizeof(info);

    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
        return {{0.f, 0.f, 1280.f, 800.f}, {0.f, 0.f, 1280.f, 800.f}, scale};

    // rcWork is rcMonitor minus the taskbar and any registered appbars.
    return {toScreenPoints(info.rcMonitor, scale),
            toScreenPoints(info.rcWork, scale),
            scale};
}
} // namespace eacp::Graphics
