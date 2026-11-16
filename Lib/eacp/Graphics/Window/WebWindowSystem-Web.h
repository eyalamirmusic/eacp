#pragma once

// The JS half of the web window system (Window-Web.js): a page's DOM, which
// under node is not there at all. Every query answers as a page with nothing
// in it would when there is no document.

extern "C"
{
    int eacp_web_window_has_dom();

    // Takes Module.canvas the first time, else makes a <canvas> of its own,
    // and returns its CSS selector as a malloc'd string, null with no DOM.
    char* eacp_web_window_create(int id, double width, double height);
    void eacp_web_window_destroy(int id);

    void eacp_web_window_set_css_size(int id, double width, double height);
    void eacp_web_window_set_backing_size(int id, int pixelWidth, int pixelHeight);
    void eacp_web_window_set_visible(int id, int visible);
    void eacp_web_window_set_title(int id, const char* title);
    void eacp_web_window_set_cursor(int id, const char* cursor);
    void eacp_web_window_set_full_screen(int id, int fullScreen);
    void eacp_web_window_set_pointer_lock(int id, int locked);

    double eacp_web_device_pixel_ratio();

    // The screen's size and the viewport's, in CSS pixels; zeroes with no DOM.
    void eacp_web_screen_metrics(double* metrics);

    int eacp_web_prefers_dark_scheme();

    // A requestAnimationFrame, or a 60 Hz timeout where there is none, that
    // calls eacp_web_frame(id) once. A second request while one is pending
    // does nothing.
    void eacp_web_frame_request(int id);
    void eacp_web_frame_cancel(int id);
}

namespace eacp::Graphics
{
// No document (node), or a headless app environment: windows are surfaceless.
bool webHasDisplay();

float webDevicePixelRatio();
} // namespace eacp::Graphics
