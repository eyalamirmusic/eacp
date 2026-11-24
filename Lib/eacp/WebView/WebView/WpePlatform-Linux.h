#pragma once

#include <cstdint>
#include <functional>

typedef struct _WPEDisplay WPEDisplay;
typedef struct _WPEToplevel WPEToplevel;
typedef struct _WPEView WPEView;

namespace eacp::Graphics::detail
{
// One rendered frame as WPE handed it over, valid only for the length of the
// call it arrives in: B, G, R, A per pixel, premultiplied, `stride` bytes a
// row, in device pixels.
struct WpeFrameBytes
{
    const std::uint8_t* bgra = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};

using WpeFrameSink = std::function<void(const WpeFrameBytes&)>;

// The process's WPE display: an in-process WPEDisplay subclass whose views
// hand every frame to a sink instead of to a window system. Connected on
// first use and kept for the life of the process; null if it would not
// connect.
WPEDisplay* wpeDisplay();

// The eacp side of the WPEView WebKit made for one web view on wpeDisplay():
// where its frames go, its size and scale, and whether it is shown and
// focused. The view stays WebKit's; the toplevel it sits in is this object's.
class WpeViewHost
{
public:
    explicit WpeViewHost(WPEView* viewToHost);
    ~WpeViewHost();

    WpeViewHost(const WpeViewHost&) = delete;
    WpeViewHost& operator=(const WpeViewHost&) = delete;

    void setFrameSink(WpeFrameSink sink);

    // While presenting, a frame is reported rendered by bufferPresented()
    // rather than by the headless frame clock, which then only stands in for
    // a present that does not come. The sink may change this for the frame
    // it is being handed.
    void setPresenting(bool presenting);
    void bufferPresented();

    // In points, with the device pixels per point. A zero size is ignored:
    // WebKit renders nothing into it.
    void resize(float widthPoints, float heightPoints, float scale);

    void setVisible(bool visible);
    void setFocused(bool focused);

private:
    WPEView* view = nullptr;
    WPEToplevel* toplevel = nullptr;
};
} // namespace eacp::Graphics::detail
