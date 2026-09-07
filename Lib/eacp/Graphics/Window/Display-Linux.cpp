#include "Display.h"

#include "WaylandDisplay-Linux.h"

namespace eacp::Graphics
{
namespace
{
// A plausible size rather than zeroes, as Display.h documents.
Display waylandFallbackDisplay()
{
    const auto frame = Rect {0.f, 0.f, 1280.f, 800.f};

    return {frame, frame, 1.f};
}
} // namespace

// Wayland names no primary display and publishes no work area.
Display primaryDisplay()
{
    auto* connection = waylandDisplay();

    if (connection == nullptr)
        return waylandFallbackDisplay();

    const auto* output = connection->getPrimaryOutput();

    if (output == nullptr)
        return waylandFallbackDisplay();

    auto size = output->logicalSize();

    // Announced, but not yet its mode.
    if (size.x <= 0.f || size.y <= 0.f)
        return waylandFallbackDisplay();

    const auto frame = Rect {output->position.x, output->position.y, size.x, size.y};

    // The output's integer scale; a surface may be told something finer.
    return {frame, frame, (float) output->scale};
}
} // namespace eacp::Graphics
