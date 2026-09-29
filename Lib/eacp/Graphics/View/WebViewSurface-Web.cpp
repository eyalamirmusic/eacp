#include "WebViewSurface-Web.h"

#include <emscripten/html5.h>

namespace eacp::Graphics
{
namespace
{
// Weak, so a frame arriving after its surface has gone finds nothing.
struct WebFrameRequest
{
    std::weak_ptr<ViewSurface*> record;
};

bool webFrameArrived(double, void* data)
{
    auto request =
        std::unique_ptr<WebFrameRequest>(static_cast<WebFrameRequest*>(data));

    auto record = request->record.lock();

    if (record == nullptr)
        return false;

    (*record)->frameCallbackPending = false;
    (*record)->onFrameDone();

    return false;
}

class WebViewSurfaceNative : public ViewSurfaceNative
{
public:
    WebViewSurfaceNative(WebWindowSurface& windowToUse,
                         ViewSurface& recordToUse,
                         ViewSurfaceNative*& holderToUse)
        : window(windowToUse)
        , record(recordToUse)
        , holder(holderToUse)
    {
        holder = this;
    }

    ~WebViewSurfaceNative() override
    {
        if (holder == this)
            holder = nullptr;
    }

    // The canvas is the whole window's, whatever the view's bounds.
    bool applyGeometry() override
    {
        auto changed = window.pixelWidth != record.pixelWidth
                       || window.pixelHeight != record.pixelHeight
                       || window.scale != record.scale;

        record.pixelWidth = window.pixelWidth;
        record.pixelHeight = window.pixelHeight;
        record.scale = window.scale;

        return changed;
    }

    // requestAnimationFrame stands in for wl_surface.frame.
    void requestFrame() override
    {
        record.frameCallbackPending = true;
        emscripten_request_animation_frame(webFrameArrived,
                                           new WebFrameRequest {target});
    }

private:
    WebWindowSurface& window;
    ViewSurface& record;
    ViewSurfaceNative*& holder;

    std::shared_ptr<ViewSurface*> target = std::make_shared<ViewSurface*>(&record);
};

class WebViewSurfaceBackend : public ViewSurfaceBackend
{
public:
    explicit WebViewSurfaceBackend(WebWindowSurface& windowToUse)
        : window(windowToUse)
    {
    }

    std::unique_ptr<ViewSurfaceNative> createSurface(View&,
                                                     ViewSurface& record) override
    {
        if (holder != nullptr || !window.nativeSurface.isValid())
            return nullptr;

        auto native = std::make_unique<WebViewSurfaceNative>(window, record, holder);

        record.handle = window.nativeSurface;
        native->applyGeometry();

        return native;
    }

private:
    WebWindowSurface& window;

    // The one native that has the canvas, or null.
    ViewSurfaceNative* holder = nullptr;
};
} // namespace

std::unique_ptr<ViewSurfaceBackend>
    makeWebViewSurfaceBackend(WebWindowSurface& window)
{
    return std::make_unique<WebViewSurfaceBackend>(window);
}
} // namespace eacp::Graphics
