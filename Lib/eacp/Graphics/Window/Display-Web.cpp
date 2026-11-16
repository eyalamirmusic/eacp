#include "Display.h"

#include "WebWindowSystem-Web.h"

#include <array>

namespace eacp::Graphics
{
namespace
{
// A plausible size rather than zeroes, as Display.h documents.
Display webFallbackDisplay()
{
    const auto frame = Rect {0.f, 0.f, 1280.f, 800.f};

    return {frame, frame, 1.f};
}
} // namespace

// The screen is the frame; the viewport is what a canvas can cover, so it is
// the work area. Both are read live, so a resized page reads its new size.
Display primaryDisplay()
{
    auto metrics = std::array<double, 4> {};
    eacp_web_screen_metrics(metrics.data());

    if (metrics[0] <= 0.0 || metrics[1] <= 0.0)
        return webFallbackDisplay();

    auto frame = Rect {0.f, 0.f, (float) metrics[0], (float) metrics[1]};
    auto workArea = frame;

    if (metrics[2] > 0.0 && metrics[3] > 0.0)
        workArea = {0.f, 0.f, (float) metrics[2], (float) metrics[3]};

    return {frame, workArea, webDevicePixelRatio()};
}
} // namespace eacp::Graphics
