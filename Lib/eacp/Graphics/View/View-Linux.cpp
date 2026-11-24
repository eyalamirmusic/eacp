#include "View-Linux.h"

#include "View.h"
#include "../Image/Image.h"
#include "../Window/LinuxWindowSurface-Linux.h"
#include "../Window/LinuxWindowSystem-Linux.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Singleton.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

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

// These registries are never destroyed, for the reason Singleton::getImmortal
// gives: a view or window closed after main returns - from an exit handler, or
// by a static built before them - deregisters from them after every
// function-local static made since has been destroyed.
std::unordered_map<View*, LinuxViewRecordPtr>& linuxViewRecords()
{
    return Singleton::getImmortal<std::unordered_map<View*, LinuxViewRecordPtr>>();
}

std::unordered_map<View*, LinuxWindowSurface*>& linuxContentViewWindows()
{
    return Singleton::getImmortal<std::unordered_map<View*, LinuxWindowSurface*>>();
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

std::unordered_map<View*, LinuxNativeChild*>& linuxNativeChildren()
{
    return Singleton::getImmortal<std::unordered_map<View*, LinuxNativeChild*>>();
}

LinuxNativeChild* linuxFindNativeChild(View& view)
{
    auto& children = linuxNativeChildren();
    auto found = children.find(&view);

    return found == children.end() ? nullptr : found->second;
}

bool linuxEffectivelyVisible(View& view)
{
    for (auto* node = &view; node != nullptr; node = node->getParent())
        if (!node->isVisible())
            return false;

    return true;
}

// A new child starts on top of its siblings, so one made for the content after
// the menu bar's would cover an open menu: the bar's go back above it.
void linuxKeepMenuBarOnTop(View& view,
                           LinuxWindowSurface& window,
                           const NativeSurfaceHandle& created)
{
    auto* bar = window.menuBarView;

    if (bar == nullptr || &linuxRootOf(view) == bar)
        return;

    for (auto& [other, state]: linuxViewRecords())
        if (state->native != nullptr && &linuxRootOf(*other) == bar)
            state->native->placeAbove(created);
}

void linuxCreateViewSurface(LinuxViewRecord& state,
                            View& view,
                            LinuxWindowSurface& window)
{
    state.native = window.viewSurfaces->createSurface(view, state.record);

    if (state.native == nullptr)
        return;

    linuxKeepMenuBarOnTop(view, window, state.record.handle);

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
    if (auto* child = linuxFindNativeChild(view))
        child->sync();

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
    if (auto* child = linuxFindNativeChild(view))
        child->release();

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
        ownerView->resizeStarted();
        ownerView->resized();
        ownerView->resizeFinished();
    }

    View* ownerView;
    Rect bounds;
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
    linuxNativeChildren().erase(this);

    // A root dying first would leave the window holding a pointer.
    auto& windows = linuxContentViewWindows();
    auto owner = windows.find(this);

    if (owner != windows.end())
    {
        auto& window = *owner->second;

        if (window.contentView == this)
            window.contentView = nullptr;

        if (window.menuBarView == this)
            window.menuBarView = nullptr;

        window.forgetRoot(this);
        windows.erase(owner);
    }

    for (auto* layer: getLayers())
        layer->detachFromLayer();

    removeFromParent();
    detachSubviews();
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

    if (window == nullptr
        || (window->contentView != root && window->menuBarView != root))
        return {};

    auto origin = linuxViewOriginInWindow(*this);
    auto position = linuxPointerPosition();

    return {position.x - origin.x, position.y - origin.y};
}

// Applied at once, so a shape set from a mouseMoved handler takes effect now.
void View::setMouseCursor(const MouseCursor& cursor)
{
    currentCursor = cursor;

    linuxRefreshCursor();
}

View& View::setAcceptsFileDrops(bool value)
{
    properties.acceptsFileDrops = value;
    return *this;
}

bool View::startFileDrag(const Vector<std::string>&, const FileDragOptions&)
{
    return false;
}

void View::focus()
{
    takeKeyFocus();
    linuxViewFocused(properties.wantsTextInput);
}

bool View::hasFocus() const
{
    return holdsKeyFocus();
}

void notifyBackingScaleChanged(View& view)
{
    view.resizeStarted();
    view.backingScaleChanged();
    view.resizeFinished();

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
    auto& windows = linuxContentViewWindows();

    if (auto found = windows.find(&contentView); found != windows.end())
        found->second->forgetRoot(&contentView);

    linuxReleaseViewSurfaceTree(contentView);
    windows.erase(&contentView);
}

void linuxUnbindWindow(LinuxWindowSurface& window)
{
    auto roots = Vector<View*> {};

    for (auto& [root, owner]: linuxContentViewWindows())
        if (owner == &window)
            roots.add(root);

    for (auto* root: roots)
        linuxUnbindWindowFromContentView(*root);
}

void linuxWindowSurfaceStateChanged(View& contentView)
{
    auto* window = linuxWindowForView(contentView);

    if (window == nullptr)
    {
        linuxSyncViewSurfaces(contentView);
        return;
    }

    auto roots = Vector<View*> {};

    for (auto& [root, owner]: linuxContentViewWindows())
        if (owner == window)
            roots.add(root);

    for (auto* root: roots)
        linuxSyncViewSurfaces(*root);
}

LinuxWindowSurface* linuxWindowForView(View& view)
{
    auto& windows = linuxContentViewWindows();
    auto found = windows.find(&linuxRootOf(view));

    return found == windows.end() ? nullptr : found->second;
}

void linuxRegisterNativeChild(View& view, LinuxNativeChild& child)
{
    linuxNativeChildren()[&view] = &child;
}

void linuxUnregisterNativeChild(View& view)
{
    linuxNativeChildren().erase(&view);
}

void linuxKeepMenuBarAbove(View& view, const NativeSurfaceHandle& created)
{
    if (auto* window = linuxWindowForView(view))
        linuxKeepMenuBarOnTop(view, *window, created);
}

Point linuxViewOriginInWindow(const View& view)
{
    auto origin = Point {};

    // The root included: its bounds are where it sits in the window, which
    // for a content view under a menu bar is not the top.
    for (const auto* node = &view; node != nullptr; node = node->getParent())
    {
        auto bounds = node->getBounds();
        origin.x += bounds.x;
        origin.y += bounds.y;
    }

    return origin;
}

namespace
{
struct LinuxAsyncTarget
{
    View* view = nullptr;
    Point offset;
    float opacity = 1.f;
};

void collectLinuxAsyncContent(View& view,
                              Point offset,
                              float opacity,
                              std::vector<LinuxAsyncTarget>& out)
{
    if (!view.isVisible())
        return;

    auto effectiveOpacity = opacity * view.getOpacity();

    if (view.hasAsyncContent())
        out.push_back({&view, offset, effectiveOpacity});

    for (auto* child: view.getSubviews())
    {
        auto bounds = child->getBounds();
        collectLinuxAsyncContent(*child,
                                 {offset.x + bounds.x, offset.y + bounds.y},
                                 effectiveOpacity,
                                 out);
    }
}

// Source-over, `source` stretched across `dest` (in the target's pixels) by
// nearest sampling.
void blendLinuxImage(ImageData& target,
                     int targetWidth,
                     int targetHeight,
                     const Image& source,
                     Rect dest,
                     float opacity)
{
    if (!source.isValid() || dest.w <= 0.f || dest.h <= 0.f)
        return;

    auto left = std::max(0, (int) std::floor(dest.x));
    auto top = std::max(0, (int) std::floor(dest.y));
    auto right = std::min(targetWidth, (int) std::ceil(dest.x + dest.w));
    auto bottom = std::min(targetHeight, (int) std::ceil(dest.y + dest.h));
    const auto& pixels = source.pixels();

    for (auto y = top; y < bottom; ++y)
    {
        auto sy = std::clamp((int) ((y + 0.5f - dest.y) * source.height() / dest.h),
                             0,
                             source.height() - 1);

        for (auto x = left; x < right; ++x)
        {
            auto sx =
                std::clamp((int) ((x + 0.5f - dest.x) * source.width() / dest.w),
                           0,
                           source.width() - 1);

            const auto* from =
                pixels.data() + ((std::size_t) sy * source.width() + sx) * 4;
            auto* to = target.data() + ((std::size_t) y * targetWidth + x) * 4;

            auto alpha = from[3] / 255.f * opacity;
            auto below = to[3] / 255.f;
            auto outAlpha = alpha + below * (1.f - alpha);

            for (auto channel = 0; channel < 3; ++channel)
            {
                auto mixed = outAlpha > 0.f ? (from[channel] * alpha
                                               + to[channel] * below * (1.f - alpha))
                                                  / outAlpha
                                            : 0.f;
                to[channel] =
                    (std::uint8_t) std::lround(std::clamp(mixed, 0.f, 255.f));
            }

            to[3] = (std::uint8_t) std::lround(outAlpha * 255.f);
        }
    }
}

struct LinuxAsyncComposite
{
    ImageData pixels;
    int width = 0;
    int height = 0;
    int remaining = 0;
    Threads::AsyncPromise<Image> promise;
};
} // namespace

// No compositing pass: a plain View snapshots as an invalid Image.
Image View::renderToImage(float scale)
{
    auto resolvedScale = scale > 0.0f ? scale : linuxDefaultBackingScale;

    return renderNativeContent(resolvedScale);
}

// The synchronous image, with every embedded web view's snapshot laid over it
// at its place in the subtree.
Threads::Async<Image> View::renderToImageAsync(float scale)
{
    auto resolvedScale = scale > 0.0f ? scale : linuxDefaultBackingScale;
    auto state = std::make_shared<LinuxAsyncComposite>();
    auto result = state->promise.get();

    auto targets = std::vector<LinuxAsyncTarget> {};
    collectLinuxAsyncContent(*this, {0.f, 0.f}, 1.f, targets);

    auto base = renderToImage(resolvedScale);

    if (targets.empty())
    {
        state->promise.resolve(base);
        return result;
    }

    auto bounds = getLocalBounds();
    state->width = std::max(1, (int) std::lround(bounds.w * resolvedScale));
    state->height = std::max(1, (int) std::lround(bounds.h * resolvedScale));

    if (base.isValid() && base.width() == state->width
        && base.height() == state->height)
        state->pixels = base.pixels();
    else
        state->pixels.resize(state->width * state->height * 4);

    state->remaining = (int) targets.size();

    for (auto& target: targets)
    {
        auto viewBounds = target.view->getBounds();
        auto dest = Rect {target.offset.x * resolvedScale,
                          target.offset.y * resolvedScale,
                          viewBounds.w * resolvedScale,
                          viewBounds.h * resolvedScale};

        target.view->captureAsyncContent(
            resolvedScale,
            [state, dest, opacity = target.opacity](Image image)
            {
                blendLinuxImage(state->pixels,
                                state->width,
                                state->height,
                                image,
                                dest,
                                opacity);

                if (--state->remaining == 0)
                    state->promise.resolve(Image {
                        state->width, state->height, std::move(state->pixels)});
            });
    }

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
