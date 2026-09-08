#include "X11ViewSurface-Linux.h"

#include "View.h"
#include "../Window/X11Connection-Linux.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Threads/Timer.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

// File-scope names carry an x11/X11 prefix: this is one unity TU under
// EACP_CI_BUILD.

namespace eacp::Graphics
{
namespace
{
constexpr int x11FallbackRefreshHz = 60;

// Outside this, the mode the server reports is not worth pacing against.
constexpr int x11MinRefreshHz = 24;
constexpr int x11MaxRefreshHz = 480;

int x11PacerHz()
{
    auto* connection = x11Connection();

    if (connection == nullptr)
        return x11FallbackRefreshHz;

    const auto& output = connection->getPrimaryOutput();

    if (!output)
        return x11FallbackRefreshHz;

    // Zero wherever there is no RandR mode to read, as on Xvfb.
    auto hz = output->refreshMilliHz / 1000;

    if (hz < x11MinRefreshHz || hz > x11MaxRefreshHz)
        return x11FallbackRefreshHz;

    return hz;
}

// X11 has no per-surface frame signal, so one timer at the output's rate
// answers every armed record. It exists only while something is armed:
// nothing presenting on this connection means no thread ticking.
class X11FramePacer
{
public:
    void arm(ViewSurface& record)
    {
        if (std::find(armed.begin(), armed.end(), &record) != armed.end())
            return;

        armed.push_back(&record);

        if (timer == nullptr)
            timer =
                std::make_unique<Threads::Timer>([this] { tick(); }, x11PacerHz());
    }

    void disarm(ViewSurface& record)
    {
        std::erase(armed, &record);

        // Mid-tick, with this record still waiting its turn: a native
        // destroyed by another record's onFrameDone takes its own entry out of
        // the batch rather than leaving a dangling one behind.
        if (firing != nullptr)
            std::replace(firing->begin(),
                         firing->end(),
                         &record,
                         static_cast<ViewSurface*>(nullptr));
    }

private:
    void tick()
    {
        auto batch = std::move(armed);
        armed.clear();

        firing = &batch;

        for (auto* record: batch)
        {
            if (record == nullptr)
                continue;

            record->frameCallbackPending = false;
            record->onFrameDone();
        }

        firing = nullptr;

        // Deferred, because the timer being dropped is the one whose callback
        // this is; a record re-armed from onFrameDone above keeps it alive.
        if (armed.empty())
            Threads::callAsync([this] { stopIfIdle(); });
    }

    void stopIfIdle()
    {
        if (armed.empty())
            timer.reset();
    }

    std::vector<ViewSurface*> armed;
    std::vector<ViewSurface*>* firing = nullptr;
    std::unique_ptr<Threads::Timer> timer;
};

// Leaked for the reason the connection is: a Timer's destructor asserts the
// main thread, and static teardown is not on it.
X11FramePacer& x11FramePacer()
{
    static auto* pacer = new X11FramePacer();
    return *pacer;
}

struct X11ChildGeometry
{
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
};

int x11ToPixels(float points, float scale)
{
    return std::max((int) std::lround(points * scale), 1);
}

class X11ViewSurfaceNative : public ViewSurfaceNative
{
public:
    X11ViewSurfaceNative(View& viewToUse,
                         X11WindowSurface& windowToUse,
                         ViewSurface& recordToUse)
        : view(viewToUse)
        , window(windowToUse)
        , record(recordToUse)
    {
    }

    ~X11ViewSurfaceNative() override
    {
        x11FramePacer().disarm(record);

        auto* connection = x11Connection();

        if (child == XCB_NONE || connection == nullptr)
            return;

        connection->unregisterWindow(child);

        if (!connection->isConnected())
            return;

        xcb_destroy_window(connection->getConnection(), child);
        connection->flush();
    }

    bool create()
    {
        auto* connection = x11Connection();

        if (connection == nullptr || !connection->isConnected())
            return false;

        auto parent = window.getWindow();

        if (parent == XCB_NONE)
            return false;

        auto* xcb = connection->getConnection();
        auto geometry = pixelGeometry();

        child = xcb_generate_id(xcb);

        // No background pixmap, so a resize shows the pixels that are already
        // there rather than a flash of the server's fill, and north-west
        // gravity keeps them anchored until the presenter catches up. Exposure
        // and nothing else: a pointer or key mask here would swallow the event
        // the toplevel wants, in this child's coordinates.
        const uint32_t values[] = {
            XCB_BACK_PIXMAP_NONE, XCB_GRAVITY_NORTH_WEST, XCB_EVENT_MASK_EXPOSURE};

        xcb_create_window(xcb,
                          XCB_COPY_FROM_PARENT,
                          child,
                          parent,
                          x11ClampPosition(geometry.x),
                          x11ClampPosition(geometry.y),
                          x11ClampSize(geometry.width),
                          x11ClampSize(geometry.height),
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          XCB_COPY_FROM_PARENT,
                          XCB_CW_BACK_PIXMAP | XCB_CW_BIT_GRAVITY
                              | XCB_CW_EVENT_MASK,
                          values);

        // With the view in the record, an Expose on this child reaches the
        // presenter through the toplevel's handleEvent.
        connection->registerWindow({child, &window, &view});

        xcb_map_window(xcb, child);

        record.handle = {NativeSurfaceHandle::Kind::X11, xcb, nullptr, child};
        record.pixelWidth = geometry.width;
        record.pixelHeight = geometry.height;
        record.scale = window.scale;

        connection->flush();

        return true;
    }

    // The record is what the view is drawn against, so it is brought up to
    // date whether or not there is a server left to tell; only the request is
    // skipped.
    bool applyGeometry() override
    {
        auto geometry = pixelGeometry();
        auto* connection = x11Connection();

        if (child != XCB_NONE && connection != nullptr && connection->isConnected())
        {
            const uint32_t values[] = {
                (uint32_t) (int32_t) x11ClampPosition(geometry.x),
                (uint32_t) (int32_t) x11ClampPosition(geometry.y),
                (uint32_t) x11ClampSize(geometry.width),
                (uint32_t) x11ClampSize(geometry.height)};

            xcb_configure_window(connection->getConnection(),
                                 child,
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
                                     | XCB_CONFIG_WINDOW_WIDTH
                                     | XCB_CONFIG_WINDOW_HEIGHT,
                                 values);
            connection->flush();
        }

        auto changed = geometry.width != record.pixelWidth
                       || geometry.height != record.pixelHeight
                       || window.scale != record.scale;

        record.pixelWidth = geometry.width;
        record.pixelHeight = geometry.height;
        record.scale = window.scale;

        return changed;
    }

    void requestFrame() override
    {
        record.frameCallbackPending = true;
        x11FramePacer().arm(record);
    }

private:
    X11ChildGeometry pixelGeometry() const
    {
        auto bounds = view.getBounds();
        auto origin = linuxViewOriginInWindow(view);
        auto scale = window.scale;

        return {(int) std::lround(origin.x * scale),
                (int) std::lround(origin.y * scale),
                x11ToPixels(bounds.w, scale),
                x11ToPixels(bounds.h, scale)};
    }

    View& view;
    X11WindowSurface& window;
    ViewSurface& record;

    xcb_window_t child = XCB_NONE;
};

class X11ViewSurfaceBackend : public ViewSurfaceBackend
{
public:
    explicit X11ViewSurfaceBackend(X11WindowSurface& windowToUse)
        : window(windowToUse)
    {
    }

    std::unique_ptr<ViewSurfaceNative> createSurface(View& view,
                                                     ViewSurface& record) override
    {
        auto native = std::make_unique<X11ViewSurfaceNative>(view, window, record);

        if (!native->create())
            return {};

        return native;
    }

private:
    X11WindowSurface& window;
};
} // namespace

std::unique_ptr<ViewSurfaceBackend>
    makeX11ViewSurfaceBackend(X11WindowSurface& window)
{
    return std::make_unique<X11ViewSurfaceBackend>(window);
}
} // namespace eacp::Graphics
