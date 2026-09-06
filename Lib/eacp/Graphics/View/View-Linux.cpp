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

// The Linux View: the portable tree, plus a wl_subsurface for the one kind of
// view that puts its own pixels on screen.
//
// Everything a View is made of that matters to app code - the tree, the bounds,
// the hit-testing, the mouse routing, the visibility bookkeeping - is portable
// and lives in View.cpp. What a backend adds is a native surface per view, and
// Linux gives one to exactly the views that ask (requestViewSurface, which is
// GPUView and nothing else). Every other view is a node in the tree with no
// surface at all, which is the Windows shape: one surface per Window, none per
// View, and all input routed by the portable hit-tester.
//
// The subsurface bookkeeping is a map keyed on the View rather than a field on
// View::Native, for the same reason CompositionHostWindow-Windows.cpp keeps its
// content-view-to-HWND map outside the view: View::Native is private to the
// class, and the window backend has to reach this from a file that cannot name
// it. The map also means a view that never presents costs nothing.

namespace eacp::Graphics
{
namespace
{
// One presenting view's Wayland state: the record the GPU module reads, and the
// objects the window backend made for it.
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

// Shared rather than unique so a deferred repaint can hold a weak reference:
// View::repaint coalesces through callAsync, and the view may be destroyed
// before the loop gets round to it.
using WaylandViewRecordPtr = std::shared_ptr<WaylandViewRecord>;

std::unordered_map<View*, WaylandViewRecordPtr>& waylandViewRecords()
{
    static auto records = std::unordered_map<View*, WaylandViewRecordPtr> {};
    return records;
}

// The window a content view belongs to. Any view finds it by walking up to the
// root, which is what makes a subtree moved between windows need no bookkeeping.
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

// The view's own flag and every ancestor's. A view hidden inside a hidden
// parent is hidden however its own flag reads, which is what View.h means by
// effective visibility and what visibilityChanged reports.
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

// Position, size and scale, in the two shapes a compositor may want them.
//
// With wp_viewporter the buffer is in real pixels and the surface's size in
// points is set independently, which is the only way a fractional scale can be
// expressed at all - 1.25x of a 100-point view is 125 pixels, and no integer
// buffer scale describes that. Without it, the buffer scale is a whole number
// and the pixel size follows from it.
//
// Returns true when the pixel size or the scale changed, which is what
// ViewSurface::onResized reports.
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

    // A subsurface's position is applied by its PARENT's commit, not its own,
    // so a view that moved is still where it was until this happens.
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

    // Desync, so the presenter's commits reach the screen on their own. A
    // synchronised subsurface only shows what its parent's next commit lets
    // through, which would put a swapchain's frame rate in the window's hands.
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

    // First, and while everything is still alive: a swapchain outliving the
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

// The whole reconciliation, for one view: what it should have against what it
// has. Everything that can change any of the inputs - the window mapping, the
// window's scale, the view's bounds, its visibility, its place in the tree -
// ends here.
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
    // Fires ViewSurface::onLost while the surface is still up, which is the
    // contract's promise for a view destroyed out from under a presenter.
    if (auto* state = waylandFindViewRecord(*this))
        waylandDestroyViewSurface(*state);

    waylandViewRecords().erase(this);

    // A content view outliving its window is the ordinary case and the window
    // clears itself out of here; a content view dying FIRST is not, and the
    // window would be left holding a pointer to it.
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

// There is no native object behind a plain Linux View, so both of these hand
// back the View::Native itself: a stable, non-null identity for this view. A
// view that presents its own pixels gets a wl_surface through
// requestViewSurface instead, which is a different thing with a different
// lifetime and is deliberately not handed out here.
void* View::getHandle()
{
    return impl.get();
}

void* View::getNativeLayer()
{
    return impl.get();
}

// On-demand rendering. A plain view has nothing to repaint - there is no 2D
// context on Linux and nothing composites a view tree - so this is only ever
// the presenting view's render path, coalesced so any number of calls in one
// turn of the loop become one, made from the loop after the caller returns.
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

    // The whole subtree, not only the branches notifyVisibilityChanged walked:
    // a view hidden in its own right already has no surface, and asking again
    // costs a map lookup.
    waylandSyncViewSurfaces(*this);
}

Rect View::getBounds() const
{
    return impl->getBounds();
}

void View::setBounds(const Rect& bounds)
{
    impl->setBounds(bounds);

    // Every descendant, because a subsurface's position is the sum of the whole
    // parent chain: moving a container moves everything presenting inside it.
    waylandSyncViewSurfaces(*this);
}

// Where the pointer is in this view's own coordinates, or the origin when it is
// somewhere else entirely - which includes every machine with no pointing
// device, since a headless compositor advertises no seat at all.
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

// Remembered, and acted on at once when the pointer is already inside this
// window. Setting the shape from inside a mouseMoved handler is the case
// View.h calls out, and it only works if the change takes effect on that same
// move rather than on the next one.
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

        // The two hooks the presenter calls rather than sets. Both hold a weak
        // reference, so one captured by a lambda that outlives the view finds
        // nothing rather than a dangling record.
        auto weak = std::weak_ptr<WaylandViewRecord>(state);

        state->record.requestFrameCallback = [weak]
        {
            auto pending = weak.lock();

            if (pending == nullptr || pending->surface == nullptr
                || pending->record.frameCallbackPending)
                return;

            // Queued on the surface, and carried by the surface's NEXT commit -
            // which the swapchain's present makes. Nothing here commits: the
            // presenter owns the subsurface's buffer queue.
            pending->frameCallback = wl_surface_frame(pending->surface);
            wl_callback_add_listener(
                pending->frameCallback, &waylandFrameListener, pending.get());

            pending->record.frameCallbackPending = true;
        };

        found = records.emplace(&view, std::move(state)).first;

        // The view may already be in a window that is up, but the presenter is
        // still inside the call that made this record and has not set its hooks
        // yet - so the surface is made a turn of the loop later, where an
        // onAvailable it installs on the way out is the one that fires. Every
        // other route in (the view joining a tree, the window mapping) is an
        // event of its own and needs no such delay.
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

    // Stops before the root, whose own bounds are the window's content rect and
    // whose origin is therefore the origin everything else is measured from.
    for (const auto* node = &view; node->getParent() != nullptr;
         node = node->getParent())
    {
        auto bounds = node->getBounds();
        origin.x += bounds.x;
        origin.y += bounds.y;
    }

    return origin;
}

// Straight to the view's own native content, with no compositing pass around
// it. On macOS and Windows this walks the tree drawing paint(), the attached
// layers and each child through a 2D context; there is no 2D context on Linux,
// so what a snapshot can honestly contain is what the view renders itself —
// which is the GPU read-back path, and the one the GPU tests ride on.
//
// A plain View therefore snapshots as an invalid Image (View::renderNativeContent
// returns one), the same answer it gives for a zero-sized view.
Image View::renderToImage(float scale)
{
    auto resolvedScale = scale > 0.0f ? scale : linuxDefaultBackingScale;

    return renderNativeContent(resolvedScale);
}

Threads::Async<Image> View::renderToImageAsync(float scale)
{
    auto promise = Threads::AsyncPromise<Image> {};
    auto result = promise.get();

    // Nothing here is asynchronous: the async form exists for embedded web
    // content, which needs a native web runtime Linux has none of.
    promise.resolve(renderToImage(scale));

    return result;
}

// A view joining the tree may now be inside a mapped window, and one leaving it
// certainly is not: both are reconciled by the same walk, which is also what
// keeps a presenting view's subsurface following its container.
void View::viewAdded(View& view)
{
    waylandSyncViewSurfaces(view);
}

void View::viewRemoved(View& view)
{
    waylandReleaseViewSurfaceTree(view);
}

} // namespace eacp::Graphics
