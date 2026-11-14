#include "LinuxWindowNative-Linux.h"

#include "../Menu/LinuxMenuBar-Linux.h"

#include <algorithm>
#include <cmath>

namespace eacp::Graphics
{
namespace
{
bool linuxFlagSet(const WindowOptions& options, WindowFlags flag)
{
    return options.flags.contains(flag);
}

void linuxNotifyHostVisibility(View* view, bool visible)
{
    if (view == nullptr)
        return;

    view->hostWindowVisibilityChanged(visible);

    for (auto* child: view->getSubviews())
        linuxNotifyHostVisibility(child, visible);
}
} // namespace

LinuxWindowState::LinuxWindowState(LinuxWindowSurface& surfaceToUse,
                                   const WindowOptions& options,
                                   WindowEvents& eventsToUse)
    : surface(surfaceToUse)
    , title(options.title)
    , quitCallback(options.effectiveOnQuit())
    , onResize(options.onResize)
    , sizeConstraint(options.effectiveSizeConstraint())
    , events(&eventsToUse)
    , minWidth(options.minWidth)
    , minHeight(options.minHeight)
    , aspectRatioHint(options.hasAspectRatio() ? options.aspectRatio
                                               : std::optional<Point> {})
    , hidesOnClose(options.hidesOnClose)
    , resizable(linuxFlagSet(options, WindowFlags::Resizable))
    , closable(linuxFlagSet(options, WindowFlags::Closable))
    , miniaturizable(linuxFlagSet(options, WindowFlags::Miniaturizable))
    , transparent(options.transparentBackground)
    , background(options.backgroundColor.value_or(linuxDefaultWindowBackground))
{
    surface.contentSize = options.effectiveInitialSize();

    if (options.initialPosition)
        position = *options.initialPosition;

    if (transparent)
        background = Color {0.f, 0.f, 0.f, 0.f};
}

LinuxWindowState::~LinuxWindowState() = default;

void LinuxWindowState::applyConstraints(int& width, int& height, bool bounded) const
{
    auto top = (int) std::lround(surface.contentTop);
    auto proposed = Point {(float) width, (float) std::max(height - top, 1)};

    auto allowed = bounded ? fitWithin(sizeConstraint, proposed)
                           : sizeConstraint({proposed, ResizeAxis::Both});

    width = std::max((int) std::lround(allowed.x), minimumSurfaceWidth());
    height = std::max((int) std::lround(allowed.y) + top, minimumSurfaceHeight());
}

void LinuxWindowState::resizeTo(Point newSize)
{
    if (newSize.x == surface.contentSize.x && newSize.y == surface.contentSize.y)
        return;

    surface.contentSize = newSize;
    surface.layoutViews();

    if (onResize)
    {
        auto content = getContentSize();
        onResize((int) content.x, (int) content.y);
    }
}

Point LinuxWindowState::getContentSize() const
{
    auto bounds = surface.contentBounds();
    return {bounds.w, bounds.h};
}

Point LinuxWindowState::surfaceSizeFor(Point contentSize) const
{
    return {contentSize.x, contentSize.y + surface.contentTop};
}

int LinuxWindowState::minimumSurfaceWidth() const
{
    return std::max(minWidth, 1);
}

int LinuxWindowState::minimumSurfaceHeight() const
{
    return std::max(minHeight, 1) + (int) std::lround(surface.contentTop);
}

void LinuxWindowState::setActive(bool nowActive)
{
    if (active == nowActive)
        return;

    active = nowActive;
    events->input.activationChanged(active);
    events->onActivationChanged(active);
}

void LinuxWindowState::setPosition(Point newPosition)
{
    position = newPosition;
    events->onMoved(position);
}

bool LinuxWindowState::hasSystemManagedSize() const
{
    return maximized || fullScreen;
}

void LinuxWindowState::notifyHostVisibility(bool visible)
{
    linuxNotifyHostVisibility(surface.contentView, visible);
    linuxNotifyHostVisibility(surface.menuBarView, visible);
}

void LinuxWindowState::closeRequested()
{
    if (hidesOnClose)
    {
        unmap();
        events->onHidden();
        return;
    }

    quitCallback();
}

void LinuxWindowNative::setContentView(View* view)
{
    auto& windowSurface = getWindowSurface();
    windowSurface.forgetRoot(windowSurface.contentView);
    windowSurface.contentView = view;

    if (view == nullptr)
        return;

    view->setBounds(windowSurface.contentBounds());

    linuxBindWindowToContentView(*view, windowSurface);

    if (windowSurface.nativeSurface.isValid())
        setVisible(true);
}

void LinuxWindowNative::setPosition(Point newPosition)
{
    getState().setPosition(newPosition);
}

void LinuxWindowNative::setContentTop(float top)
{
    auto& state = getState();
    auto& windowSurface = getWindowSurface();

    if (top == windowSurface.contentTop)
    {
        windowSurface.layoutViews();
        return;
    }

    auto content = state.getContentSize();
    auto surfaceBefore = windowSurface.contentSize;
    windowSurface.contentTop = top;

    sizeHintsChanged();

    // Laid out first, so a window whose size is the system's still moves its
    // content down.
    windowSurface.layoutViews();
    setSize(state.surfaceSizeFor(content));

    // A surface that kept its size took the bar out of the content, which
    // the content's owner hears about as a resize.
    auto kept = windowSurface.contentSize.x == surfaceBefore.x
                && windowSurface.contentSize.y == surfaceBefore.y;

    if (kept && state.onResize)
    {
        auto now = state.getContentSize();
        state.onResize((int) now.x, (int) now.y);
    }
}

void* LinuxWindowNative::getContentViewHandle()
{
    auto* view = getWindowSurface().contentView;
    return view != nullptr ? view->getHandle() : nullptr;
}

LinuxWindowSurface& LinuxWindowNative::getWindowSurface()
{
    return getState().surface;
}

bool LinuxWindowNative::isVisible()
{
    return getWindowSurface().mapped;
}

bool LinuxWindowNative::isFullScreen()
{
    return getState().fullScreen;
}

Point LinuxWindowNative::getPosition()
{
    return getState().position;
}

bool LinuxWindowNative::isMouseLocked()
{
    return getWindowSurface().mouseLockIntent;
}
} // namespace eacp::Graphics
