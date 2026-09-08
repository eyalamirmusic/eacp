#include "View-Linux.h"

#include "View.h"
#include "../Image/Image.h"
#include "../Window/LinuxWindowSurface-Linux.h"
#include "../Window/LinuxWindowSystem-Linux.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Threads/EventLoop.h>

#include <memory>
#include <unordered_map>

namespace eacp::Graphics
{
namespace
{
struct LinuxViewRecord
{
    ViewSurface record;

    View* view = nullptr;

    // The window system's child surface; null while the view has none.
    std::unique_ptr<ViewSurfaceNative> native;

    bool repaintPending = false;
};

// Shared so a deferred repaint can hold a weak reference to it.
using LinuxViewRecordPtr = std::shared_ptr<LinuxViewRecord>;

std::unordered_map<View*, LinuxViewRecordPtr>& linuxViewRecords()
{
    static auto records = std::unordered_map<View*, LinuxViewRecordPtr> {};
    return records;
}

std::unordered_map<View*, LinuxWindowSurface*>& linuxContentViewWindows()
{
    static auto windows = std::unordered_map<View*, LinuxWindowSurface*> {};
    return windows;
}

LinuxViewRecord* linuxFindViewRecord(View& view)
{
    auto& records = linuxViewRecords();
    auto found = records.find(&view);

    return found == records.end() ? nullptr : found->second.get();
}

LinuxViewRecordPtr linuxFindViewRecordPtr(View& view)
{
    auto& records = linuxViewRecords();
    auto found = records.find(&view);

    return found == records.end() ? LinuxViewRecordPtr {} : found->second;
}

View& linuxRootOf(View& view)
{
    auto* root = &view;

    while (root->getParent() != nullptr)
        root = root->getParent();

    return *root;
}

LinuxWindowSurface* linuxWindowForView(View& view)
{
    auto& windows = linuxContentViewWindows();
    auto found = windows.find(&linuxRootOf(view));

    return found == windows.end() ? nullptr : found->second;
}

bool linuxEffectivelyVisible(View& view)
{
    for (auto* node = &view; node != nullptr; node = node->getParent())
        if (!node->isVisible())
            return false;

    return true;
}

void linuxCreateViewSurface(LinuxViewRecord& state,
                            View& view,
                            LinuxWindowSurface& window)
{
    state.native = window.viewSurfaces->createSurface(view, state.record);

    if (state.native == nullptr)
        return;

    state.record.frameCallbackPending = false;
    state.record.onAvailable();
}

void linuxDestroyViewSurface(LinuxViewRecord& state)
{
    if (state.native == nullptr)
        return;

    // First, while everything is still alive: a swapchain outliving the
    // surface it was made from is a use-after-free inside the driver.
    state.record.onLost();

    state.native.reset();

    state.record.handle = {};
    state.record.pixelWidth = 0;
    state.record.pixelHeight = 0;

    // A callback that will never arrive must not hold the presenter forever.
    state.record.frameCallbackPending = false;
}

void linuxSyncOneViewSurface(View& view)
{
    auto* state = linuxFindViewRecord(view);

    if (state == nullptr)
        return;

    auto* window = linuxWindowForView(view);
    auto bounds = view.getBounds();

    auto wanted = window != nullptr && window->mapped
                  && window->nativeSurface.isValid() && linuxEffectivelyVisible(view)
                  && bounds.w > 0.f && bounds.h > 0.f;

    if (!wanted)
    {
        linuxDestroyViewSurface(*state);
        return;
    }

    if (state->native == nullptr)
    {
        linuxCreateViewSurface(*state, view, *window);
        return;
    }

    if (state->native->applyGeometry())
        state->record.onResized();
}

void linuxSyncViewSurfaces(View& view)
{
    linuxSyncOneViewSurface(view);

    for (auto* child: view.getSubviews())
        linuxSyncViewSurfaces(*child);
}

void linuxReleaseViewSurfaceTree(View& view)
{
    if (auto* state = linuxFindViewRecord(view))
        linuxDestroyViewSurface(*state);

    for (auto* child: view.getSubviews())
        linuxReleaseViewSurfaceTree(*child);
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
    if (auto* state = linuxFindViewRecord(*this))
        linuxDestroyViewSurface(*state);

    linuxViewRecords().erase(this);

    // A content view dying first would leave the window holding a pointer.
    auto& windows = linuxContentViewWindows();
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

// Not a native surface: a presenting view's comes from requestViewSurface.
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
    auto state = linuxFindViewRecordPtr(*this);

    if (state == nullptr || state->repaintPending)
        return;

    state->repaintPending = true;

    Threads::callAsync(
        [weak = std::weak_ptr<LinuxViewRecord>(state)]
        {
            auto pending = weak.lock();

            if (pending == nullptr)
                return;

            pending->repaintPending = false;

            if (pending->native != nullptr)
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

    linuxSyncViewSurfaces(*this);
}

Rect View::getBounds() const
{
    return impl->getBounds();
}

void View::setBounds(const Rect& bounds)
{
    impl->setBounds(bounds);

    // Every descendant: a child surface's position is the sum of its parent
    // chain.
    linuxSyncViewSurfaces(*this);
}

// The origin when the pointer is elsewhere, or when there is no seat at all.
Point View::getMousePosition() const
{
    auto* window = linuxPointerWindow();

    const View* root = this;

    while (root->getParent() != nullptr)
        root = root->getParent();

    if (window == nullptr || window->contentView != root)
        return {};

    auto origin = linuxViewOriginInWindow(*this);
    auto position = linuxPointerPosition();

    return {position.x - origin.x, position.y - origin.y};
}

// Applied at once, so a shape set from a mouseMoved handler takes effect now.
void View::setMouseCursor(MouseCursor cursor)
{
    currentCursor = cursor;

    linuxRefreshCursor();
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
    auto& records = linuxViewRecords();
    auto found = records.find(&view);

    if (found == records.end())
    {
        auto state = std::make_shared<LinuxViewRecord>();
        state->view = &view;

        // Weak, so a hook outliving the view finds nothing, not a dangling
        // record.
        auto weak = std::weak_ptr<LinuxViewRecord>(state);

        state->record.requestFrameCallback = [weak]
        {
            auto pending = weak.lock();

            if (pending == nullptr || pending->native == nullptr
                || pending->record.frameCallbackPending)
                return;

            pending->native->requestFrame();
        };

        found = records.emplace(&view, std::move(state)).first;

        // Deferred a turn: the caller is still inside this call and has not set
        // its hooks yet, so onAvailable would otherwise fire before it exists.
        Threads::callAsync(
            [weak = std::weak_ptr<LinuxViewRecord>(found->second)]
            {
                if (auto pending = weak.lock())
                    linuxSyncOneViewSurface(*pending->view);
            });
    }

    return found->second->record;
}

void linuxBindWindowToContentView(View& contentView, LinuxWindowSurface& window)
{
    linuxContentViewWindows()[&contentView] = &window;
    linuxSyncViewSurfaces(contentView);
}

void linuxUnbindWindowFromContentView(View& contentView)
{
    linuxReleaseViewSurfaceTree(contentView);
    linuxContentViewWindows().erase(&contentView);
}

void linuxWindowSurfaceStateChanged(View& contentView)
{
    linuxSyncViewSurfaces(contentView);
}

Point linuxViewOriginInWindow(const View& view)
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
    linuxSyncViewSurfaces(view);
}

void View::viewRemoved(View& view)
{
    linuxReleaseViewSurfaceTree(view);
}

} // namespace eacp::Graphics
