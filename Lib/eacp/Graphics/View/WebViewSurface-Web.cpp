#include "WebViewSurface-Web.h"

#include "../Helpers/AnimationFrame-Web.h"

namespace eacp::Graphics
{
namespace
{
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

    // The surface is the whole canvas, whatever the view's bounds.
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

    // Answered from inside the browser's frame callback, which is where a
    // canvas is best drawn.
    void requestFrame() override
    {
        record.frameCallbackPending = true;

        frame.request(
            [this]
            {
                record.frameCallbackPending = false;
                record.onFrameDone();
            });
    }

private:
    WebWindowSurface& window;
    ViewSurface& record;
    ViewSurfaceNative*& holder;
    AnimationFrame frame;
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
        if (holder != nullptr || window.canvasSelector.empty())
            return nullptr;

        auto native = std::make_unique<WebViewSurfaceNative>(window, record, holder);

        record.handle = window.nativeSurface;
        native->applyGeometry();

        return native;
    }

private:
    WebWindowSurface& window;

    ViewSurfaceNative* holder = nullptr;
};
} // namespace

std::unique_ptr<ViewSurfaceBackend>
    makeWebViewSurfaceBackend(WebWindowSurface& window)
{
    return std::make_unique<WebViewSurfaceBackend>(window);
}
} // namespace eacp::Graphics
