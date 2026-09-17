#include "LinuxWindowNative-Linux.h"

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

void LinuxWindowState::applyConstraints(int& width, int& height, bool bounded) const
{
    auto proposed = Point {(float) width, (float) height};

    auto allowed = bounded ? fitWithin(sizeConstraint, proposed)
                           : sizeConstraint({proposed, ResizeAxis::Both});

    width = std::max((int) std::lround(allowed.x), std::max(minWidth, 1));
    height = std::max((int) std::lround(allowed.y), std::max(minHeight, 1));
}

void LinuxWindowState::resizeTo(Point newSize)
{
    if (newSize.x == surface.contentSize.x && newSize.y == surface.contentSize.y)
        return;

    surface.contentSize = newSize;

    if (surface.contentView != nullptr)
        surface.contentView->setBounds(
            {0.f, 0.f, surface.contentSize.x, surface.contentSize.y});

    if (onResize)
        onResize((int) surface.contentSize.x, (int) surface.contentSize.y);
}

void LinuxWindowState::setActive(bool nowActive)
{
    if (active == nowActive)
        return;

    active = nowActive;
    events->onActivationChanged(active);
}

void LinuxWindowState::setPosition(Point newPosition)
{
    position = newPosition;
    events->onMoved(position);
}

void LinuxWindowState::notifyHostVisibility(bool visible)
{
    linuxNotifyHostVisibility(surface.contentView, visible);
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
    windowSurface.contentView = view;

    if (view == nullptr)
        return;

    view->setBounds(
        {0.f, 0.f, windowSurface.contentSize.x, windowSurface.contentSize.y});

    linuxBindWindowToContentView(*view, windowSurface);

    if (windowSurface.nativeSurface.isValid())
        setVisible(true);
}

void LinuxWindowNative::setPosition(Point newPosition)
{
    getState().setPosition(newPosition);
}

void* LinuxWindowNative::getContentViewHandle()
{
    auto* view = getWindowSurface().contentView;
    return view != nullptr ? view->getHandle() : nullptr;
}
} // namespace eacp::Graphics
