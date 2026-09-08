#include "Display.h"

#include "LinuxWindowSystem-Linux.h"

namespace eacp::Graphics
{
namespace
{
// A plausible size rather than zeroes, as Display.h documents.
Display linuxFallbackDisplay()
{
    const auto frame = Rect {0.f, 0.f, 1280.f, 800.f};

    return {frame, frame, 1.f};
}
} // namespace

// Linux names no primary display and publishes no work area.
Display primaryDisplay()
{
    auto output = linuxPrimaryOutput();

    if (!output)
        return linuxFallbackDisplay();

    // The output's integer scale; a surface may be told something finer.
    return {output->frame, output->frame, output->scale};
}
} // namespace eacp::Graphics
