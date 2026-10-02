#include "AndroidViewSurface-Android.h"

#include <eacp/Core/Utils/Logging.h>

#include <android/choreographer.h>

#include <utility>

namespace eacp::Graphics
{
namespace
{
// Weak, so a frame arriving after its surface has gone finds nothing.
struct AndroidFrameRequest
{
    std::weak_ptr<ViewSurface*> record;
};

void androidFrameArrived(int64_t, void* data)
{
    auto request = std::unique_ptr<AndroidFrameRequest>(
        static_cast<AndroidFrameRequest*>(data));

    auto record = request->record.lock();

    if (record == nullptr)
        return;

    (*record)->frameCallbackPending = false;
    (*record)->onFrameDone();
}

class AndroidViewSurfaceNative : public ViewSurfaceNative
{
public:
    AndroidViewSurfaceNative(AndroidWindowSurface& windowToUse,
                             ViewSurface& recordToUse,
                             ViewSurfaceNative*& holderToUse)
        : window(windowToUse)
        , record(recordToUse)
        , holder(holderToUse)
    {
        holder = this;
    }

    ~AndroidViewSurfaceNative() override
    {
        if (holder == this)
            holder = nullptr;
    }

    // The surface is the whole window's, whatever the view's bounds.
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

    void requestFrame() override
    {
        auto* choreographer = AChoreographer_getInstance();

        if (choreographer == nullptr)
        {
            static auto reported = false;

            if (!std::exchange(reported, true))
                LOG("GPUView: no AChoreographer on this thread, so no frames");

            return;
        }

        record.frameCallbackPending = true;
        AChoreographer_postFrameCallback64(
            choreographer, androidFrameArrived, new AndroidFrameRequest {target});
    }

private:
    AndroidWindowSurface& window;
    ViewSurface& record;
    ViewSurfaceNative*& holder;

    std::shared_ptr<ViewSurface*> target = std::make_shared<ViewSurface*>(&record);
};

class AndroidViewSurfaceBackend : public ViewSurfaceBackend
{
public:
    explicit AndroidViewSurfaceBackend(AndroidWindowSurface& windowToUse)
        : window(windowToUse)
    {
    }

    std::unique_ptr<ViewSurfaceNative> createSurface(View&,
                                                     ViewSurface& record) override
    {
        if (holder != nullptr || window.nativeWindow == nullptr)
            return nullptr;

        auto native =
            std::make_unique<AndroidViewSurfaceNative>(window, record, holder);

        record.handle = window.nativeSurface;
        native->applyGeometry();

        return native;
    }

private:
    AndroidWindowSurface& window;

    ViewSurfaceNative* holder = nullptr;
};
} // namespace

std::unique_ptr<ViewSurfaceBackend>
    makeAndroidViewSurfaceBackend(AndroidWindowSurface& window)
{
    return std::make_unique<AndroidViewSurfaceBackend>(window);
}
} // namespace eacp::Graphics
