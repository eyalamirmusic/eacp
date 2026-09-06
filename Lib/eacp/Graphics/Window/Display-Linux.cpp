#include "Display.h"

#include "WaylandDisplay-Linux.h"

namespace eacp::Graphics
{
namespace
{
// What Display.h documents as the answer when the platform reports nothing: a
// plausible size for an app to place its first window against, rather than
// zeroes an app would have to guard a division against. The same shape
// Display-iOS.mm falls back to when there is no screen.
Display waylandFallbackDisplay()
{
    const auto frame = Rect {0.f, 0.f, 1280.f, 800.f};

    return {frame, frame, 1.f};
}
} // namespace

// The first wl_output the compositor announced.
//
// Wayland has no notion of a primary display - there is no "the one with the
// menu bar", and a client is not told which monitor a user thinks of as theirs
// - so first-announced is as principled an answer as the protocol allows, and
// it is the one every other client uses.
//
// workArea is the frame. Wayland has no work area either: a panel or a dock is
// an ordinary client with a surface of its own, and the compositor does not
// publish the space it leaves. A desktop portal could be asked one day; until
// then, saying the whole display is available is the honest answer rather than
// an invented inset.
Display primaryDisplay()
{
    auto* connection = waylandDisplay();

    if (connection == nullptr)
        return waylandFallbackDisplay();

    const auto* output = connection->getPrimaryOutput();

    if (output == nullptr)
        return waylandFallbackDisplay();

    auto size = output->logicalSize();

    // An output that has announced itself but not yet its mode. Rare, and only
    // reachable if something asks before the connection's opening round trips
    // have completed.
    if (size.x <= 0.f || size.y <= 0.f)
        return waylandFallbackDisplay();

    const auto frame = Rect {output->position.x, output->position.y, size.x, size.y};

    // The output's own integer scale, not a surface's fractional one: this is a
    // property of the display, and a window on it may still be told something
    // finer through wp_fractional_scale.
    return {frame, frame, (float) output->scale};
}
} // namespace eacp::Graphics
