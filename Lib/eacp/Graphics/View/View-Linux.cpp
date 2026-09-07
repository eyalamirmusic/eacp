#include "View-Linux.h"

#include "View.h"
#include "WaylandViewSurface-Linux.h"
#include "../Image/Image.h"
#include "../Window/WaylandDisplay-Linux.h"
#include "../Window/WaylandInput-Linux.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Threads/EventLoop.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace eacp::Graphics
{
namespace
{
struct WaylandViewRecord
{
    ViewSurface record;

    View* view = nullptr;
    wl_surface* surface = nullptr;
    wl_subsurface* subsurface = nullptr;
    wp_viewport* viewport = nullptr;
    wl_callback* frameCallback = nullptr;

    bool repaintPending = false;
};

// Shared so a deferred repaint can hold a weak reference to it.
using WaylandViewRecordPtr = std::shared_ptr<WaylandViewRecord>;

std::unordered_map<View*, WaylandViewRecordPtr>& waylandViewRecords()
{
    static auto records = std::unordered_map<View*, WaylandViewRecordPtr> {};
    return records;
}

std::unordered_map<View*, WaylandWindowSurface*>& waylandContentViewWindows()
{
    static auto windows = std::unordered_map<View*, WaylandWindowSurface*> {};
    return windows;
}

WaylandViewRecord* waylandFindViewRecord(View& view)
{
    auto& records = waylandViewRecords();
    auto found = records.find(&view);

    return found == records.end() ? nullptr : found->second.get();
}

WaylandViewRecordPtr waylandFindViewRecordPtr(View& view)
{
    auto& records = waylandViewRecords();
    auto found = records.find(&view);

    return found == records.end() ? WaylandViewRecordPtr {} : found->second;
}

View& waylandRootOf(View& view)
{
    auto* root = &view;

    while (root->getParent() != nullptr)
        root = root->getParent();

    return *root;
}

WaylandWindowSurface* waylandWindowForView(View& view)
{
    auto& windows = waylandContentViewWindows();
    auto found = windows.find(&waylandRootOf(view));

    return found == windows.end() ? nullptr : found->second;
}

bool waylandEffectivelyVisible(View& view)
{
    for (auto* node = &view; node != nullptr; node = node->getParent())
        if (!node->isVisible())
            return false;

    return true;
}

int waylandRoundToPixels(float points, float scale)
{
    return std::max((int) std::lround(points * scale), 1);
}

void waylandFrameDone(void* data, wl_callback* callback, uint32_t)
{
    auto& state = *static_cast<WaylandViewRecord*>(data);

    if (state.frameCallback == callback)
    {
        wl_callback_destroy(state.frameCallback);
        state.frameCallback = nullptr;
    }

    state.record.frameCallbackPending = false;
    state.record.onFrameDone();
}

const wl_callback_listener waylandFrameListener {
    .done = waylandFrameDone,
};

// wp_viewporter is the only way to express a fractional scale; without it the
// buffer scale is a whole number. True when the pixel size or scale changed.
bool waylandApplyViewGeometry(WaylandViewRecord& state,
                              View& view,
                              WaylandWindowSurface& window)
{
    auto bounds = view.getBounds();
    auto origin = waylandViewOriginInWindow(view);

    wl_subsurface_set_position(state.subsurface,
                               (int32_t) std::lround(origin.x),
                               (int32_t) std::lround(origin.y));

    auto scale = window.scale;
    auto pixelWidth = 0;
    auto pixelHeight = 0;

    if (state.viewport != nullptr)
    {
        pixelWidth = waylandRoundToPixels(bounds.w, scale);
        pixelHeight = waylandRoundToPixels(bounds.h, scale);

        wp_viewport_set_destination(state.viewport,
                                    std::max((int32_t) std::lround(bounds.w), 1),
                                    std::max((int32_t) std::lround(bounds.h), 1));
    }
    else
    {
        auto wholeScale = std::max((int) std::lround(scale), 1);

        wl_surface_set_buffer_scale(state.surface, wholeScale);

        pixelWidth = std::max((int) std::lround(bounds.w), 1) * wholeScale;
        pixelHeight = std::max((int) std::lround(bounds.h), 1) * wholeScale;
        scale = (float) wholeScale;
    }

    auto changed = pixelWidth != state.record.pixelWidth
                   || pixelHeight != state.record.pixelHeight
                   || scale != state.record.scale;

    state.record.pixelWidth = pixelWidth;
    state.record.pixelHeight = pixelHeight;
    state.record.scale = scale;

    // A subsurface's position is applied by its parent's commit, not its own.
    wl_surface_commit(window.surface);

    return changed;
}

void waylandCreateViewSurface(WaylandViewRecord& state,
                              View& view,
                              WaylandWindowSurface& window)
{
    auto* connection = waylandDisplay();

    if (connection == nullptr || connection->getCompositor() == nullptr
        || connection->getSubcompositor() == nullptr)
        return;

    state.surface = wl_compositor_create_surface(connection->getCompositor());

    if (state.surface == nullptr)
        return;

    state.subsurface = wl_subcompositor_get_subsurface(
        connection->getSubcompositor(), state.surface, window.surface);

    // Desync, so the presenter's commits do not wait for the window's.
    wl_subsurface_set_desync(state.subsurface);

    if (auto* viewporter = connection->getViewporter())
        state.viewport = wp_viewporter_get_viewport(viewporter, state.surface);

    connection->registerSurface({state.surface, &window, &view});

    waylandApplyViewGeometry(state, view, window);

    state.record.display = connection->getDisplay();
    state.record.surface = state.surface;
    state.record.frameCallbackPending = false;

    state.record.onAvailable();
}

void waylandDestroyViewSurface(WaylandViewRecord& state)
{
    if (state.surface == nullptr)
        return;

    // First, while everything is still alive: a swapchain outliving the
    // wl_surface it was made from is a use-after-free inside the driver.
    state.record.onLost();

    state.record.display = nullptr;
    state.record.surface = nullptr;
    state.record.pixelWidth = 0;
    state.record.pixelHeight = 0;

    // A callback that will never arrive must not hold the presenter forever.
    state.record.frameCallbackPending = false;

    if (state.frameCallback != nullptr)
    {
        wl_callback_destroy(state.frameCallback);
        state.frameCallback = nullptr;
    }

    if (auto* connection = waylandDisplay())
        connection->unregisterSurface(state.surface);

    if (state.viewport != nullptr)
    {
        wp_viewport_destroy(state.viewport);
        state.viewport = nullptr;
    }

    if (state.subsurface != nullptr)
    {
        wl_subsurface_destroy(state.subsurface);
        state.subsurface = nullptr;
    }

    wl_surface_destroy(state.surface);
    state.surface = nullptr;
}

void waylandSyncOneViewSurface(View& view)
{
    auto* state = waylandFindViewRecord(view);

    if (state == nullptr)
        return;

    auto* window = waylandWindowForView(view);
    auto bounds = view.getBounds();

    auto wanted = window != nullptr && window->mapped && window->surface != nullptr
                  && waylandEffectivelyVisible(view) && bounds.w > 0.f
                  && bounds.h > 0.f;

    if (!wanted)
    {
        waylandDestroyViewSurface(*state);
        return;
    }

    if (state->surface == nullptr)
    {
        waylandCreateViewSurface(*state, view, *window);
        return;
    }

    if (waylandApplyViewGeometry(*state, view, *window))
        state->record.onResized();
}

void waylandSyncViewSurfaces(View& view)
{
    waylandSyncOneViewSurface(view);

    for (auto* child: view.getSubviews())
        waylandSyncViewSurfaces(*child);
}

void waylandReleaseViewSurfaceTree(View& view)
{
    if (auto* state = waylandFindViewRecord(view))
        waylandDestroyViewSurface(*state);

    for (auto* child: view.getSubviews())
        waylandReleaseViewSurfaceTree(*child);
}
} // namespace

struct View::Native
{
    explicit Native(View* owner)
        : ownerView(owner)
    {
    }

    Rect getBounds() const { return bounds; }

    void setBounds(const Rect& newBounds)
    {
        bounds = newBounds;
        ownerView->resized();
    }

    void focus() { focused = true; }
    bool hasFocus() const { return focused; }

    View* ownerView;
    Rect bounds;
    bool focused = false;
};

View::View()
    : impl(this)
{
}

View::~View()
{
    if (auto* state = waylandFindViewRecord(*this))
        waylandDestroyViewSurface(*state);

    waylandViewRecords().erase(this);

    // A content view dying first would leave the window holding a pointer.
    auto& windows = waylandContentViewWindows();
    auto owner = windows.find(this);

    if (owner != windows.end())
    {
        owner->second->contentView = nullptr;
        windows.erase(owner);
    }

    for (auto* layer: getLayers())
        layer->detachFromLayer();

    removeFromParent();
}

// Not a wl_surface: a presenting view's comes from requestViewSurface.
void* View::getHandle()
{
    return impl.get();
}

void* View::getNativeLayer()
{
    return impl.get();
}

void View::repaint()
{
    auto state = waylandFindViewRecordPtr(*this);

    if (state == nullptr || state->repaintPending)
        return;

    state->repaintPending = true;

    Threads::callAsync(
        [weak = std::weak_ptr<WaylandViewRecord>(state)]
        {
            auto pending = weak.lock();

            if (pending == nullptr)
                return;

            pending->repaintPending = false;

            if (pending->record.surface != nullptr)
                pending->record.onRepaint();
        });
}

void View::setOpacity(float opacityToUse)
{
    opacity = opacityToUse;
}

void View::setVisible(bool shouldBeVisible)
{
    if (visible == shouldBeVisible)
        return;

    visible = shouldBeVisible;
    notifyVisibilityChanged(shouldBeVisible);

    waylandSyncViewSurfaces(*this);
}

Rect View::getBounds() const
{
    return impl->getBounds();
}

void View::setBounds(const Rect& bounds)
{
    impl->setBounds(bounds);

    // Every descendant: a subsurface's position is the sum of its parent chain.
    waylandSyncViewSurfaces(*this);
}

// The origin when the pointer is elsewhere, or when there is no seat at all.
Point View::getMousePosition() const
{
    auto* connection = waylandDisplay();

    if (connection == nullptr || connection->getInput() == nullptr)
        return {};

    auto* input = connection->getInput();
    auto* window = input->getPointerWindow();

    const View* root = this;

    while (root->getParent() != nullptr)
        root = root->getParent();

    if (window == nullptr || window->contentView != root)
        return {};

    auto origin = waylandViewOriginInWindow(*this);
    auto position = input->getPointerPosition();

    return {position.x - origin.x, position.y - origin.y};
}

// Applied at once, so a shape set from a mouseMoved handler takes effect now.
void View::setMouseCursor(MouseCursor cursor)
{
    currentCursor = cursor;

    if (auto* connection = waylandDisplay())
        if (auto* input = connection->getInput())
            input->refreshCursor();
}

void View::focus()
{
    impl->focus();
}

bool View::hasFocus() const
{
    return impl->hasFocus();
}

void notifyBackingScaleChanged(View& view)
{
    view.backingScaleChanged();

    for (auto* child: view.getSubviews())
        notifyBackingScaleChanged(*child);
}

ViewSurface& requestViewSurface(View& view)
{
    auto& records = waylandViewRecords();
    auto found = records.find(&view);

    if (found == records.end())
    {
        auto state = std::make_shared<WaylandViewRecord>();
        state->view = &view;

        // Weak, so a hook outliving the view finds nothing, not a dangling
        // record.
        auto weak = std::weak_ptr<WaylandViewRecord>(state);

        state->record.requestFrameCallback = [weak]
        {
            auto pending = weak.lock();

            if (pending == nullptr || pending->surface == nullptr
                || pending->record.frameCallbackPending)
                return;

            // Carried by the surface's next commit; nothing here commits.
            pending->frameCallback = wl_surface_frame(pending->surface);
            wl_callback_add_listener(
                pending->frameCallback, &waylandFrameListener, pending.get());

            pending->record.frameCallbackPending = true;
        };

        found = records.emplace(&view, std::move(state)).first;

        // Deferred a turn: the caller is still inside this call and has not set
        // its hooks yet, so onAvailable would otherwise fire before it exists.
        Threads::callAsync(
            [weak = std::weak_ptr<WaylandViewRecord>(found->second)]
            {
                if (auto pending = weak.lock())
                    waylandSyncOneViewSurface(*pending->view);
            });
    }

    return found->second->record;
}

void waylandBindWindowToContentView(View& contentView, WaylandWindowSurface& window)
{
    waylandContentViewWindows()[&contentView] = &window;
    waylandSyncViewSurfaces(contentView);
}

void waylandUnbindWindowFromContentView(View& contentView)
{
    waylandReleaseViewSurfaceTree(contentView);
    waylandContentViewWindows().erase(&contentView);
}

void waylandWindowSurfaceStateChanged(View& contentView)
{
    waylandSyncViewSurfaces(contentView);
}

Point waylandViewOriginInWindow(const View& view)
{
    auto origin = Point {};

    // Stops before the root, whose bounds are the window's content rect.
    for (const auto* node = &view; node->getParent() != nullptr;
         node = node->getParent())
    {
        auto bounds = node->getBounds();
        origin.x += bounds.x;
        origin.y += bounds.y;
    }

    return origin;
}

// No compositing pass: a plain View snapshots as an invalid Image.
Image View::renderToImage(float scale)
{
    auto resolvedScale = scale > 0.0f ? scale : linuxDefaultBackingScale;

    return renderNativeContent(resolvedScale);
}

Threads::Async<Image> View::renderToImageAsync(float scale)
{
    auto promise = Threads::AsyncPromise<Image> {};
    auto result = promise.get();

    // The async form exists for embedded web content, which Linux has none of.
    promise.resolve(renderToImage(scale));

    return result;
}

void View::viewAdded(View& view)
{
    waylandSyncViewSurfaces(view);
}

void View::viewRemoved(View& view)
{
    waylandReleaseViewSurfaceTree(view);
}

} // namespace eacp::Graphics
