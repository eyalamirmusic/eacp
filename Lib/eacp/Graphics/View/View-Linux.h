#pragma once

namespace eacp::Graphics
{
class View;

// Pixels per point a Linux View reports while nothing has said otherwise.
inline constexpr float linuxDefaultBackingScale = 1.0f;

// Announces a backing-scale change to `view` and every view under it.
//
// Windows never calls View::backingScaleChanged at all, and gets away with it
// because a DPI change there always arrives with a WM_SIZE behind it, so
// anything sized in device pixels is rebuilt by the resize. Wayland's
// wp_fractional_scale and wl_surface.preferred_buffer_scale carry no size with
// them, so on Linux the news has to travel on its own or a glyph atlas
// rasterized at 2x goes on being drawn at 1x.
//
// Nothing calls it yet: a headless Window has no surface for a compositor to
// report a scale on. It exists so the Wayland surface listener (plan stage 4)
// has one place to plug into rather than a guess per Window backend.
void notifyBackingScaleChanged(View& view);
} // namespace eacp::Graphics
