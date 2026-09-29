#include "Display.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

namespace eacp::Graphics
{
namespace
{
// The viewport, in CSS pixels: 0 width, 1 height.
EM_JS(double, webViewportSize, (int axis), {
    return axis == 0 ? window.innerWidth : window.innerHeight;
});
} // namespace

// The screen as the page may know it, and the viewport as the space a page can
// occupy, both in CSS pixels, which are points.
Display primaryDisplay()
{
    auto width = 0;
    auto height = 0;
    emscripten_get_screen_size(&width, &height);

    auto frame = Rect {0.f, 0.f, (float) width, (float) height};
    auto workArea =
        Rect {0.f, 0.f, (float) webViewportSize(0), (float) webViewportSize(1)};
    auto scale = (float) emscripten_get_device_pixel_ratio();

    return {frame, workArea, scale > 0.f ? scale : 1.f};
}
} // namespace eacp::Graphics
