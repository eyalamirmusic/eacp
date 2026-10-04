#include "Window.h"

#include <eacp/Core/App/App.h>

#include <algorithm>

namespace eacp::Graphics
{
WindowOptions::WindowOptions()
{
    flags.add({WindowFlags::Titled,
               WindowFlags::Closable,
               WindowFlags::Miniaturizable,
               WindowFlags::Resizable});
}

WindowOptions::WindowOptions(const WindowOptions&) = default;
WindowOptions::WindowOptions(WindowOptions&&) noexcept = default;
WindowOptions& WindowOptions::operator=(const WindowOptions&) = default;
WindowOptions& WindowOptions::operator=(WindowOptions&&) noexcept = default;
WindowOptions::~WindowOptions() = default;

Callback WindowOptions::effectiveOnQuit() const
{
    if (onQuit)
        return onQuit;
    return isPrimary ? Callback {[] { Apps::quit(); }} : Callback {[] {}};
}

bool WindowOptions::effectiveAllowsFullScreen() const
{
    return allowsFullScreen.value_or(!hasAspectRatio());
}

bool WindowOptions::hasAspectRatio() const
{
    return aspectRatio && aspectRatio->x > 0.f && aspectRatio->y > 0.f;
}

SizeConstraint WindowOptions::effectiveSizeConstraint() const
{
    auto willResize = onWillResize;
    auto custom = sizeConstraint;
    auto lock = AspectRatioLock {aspectRatio.value_or(Point {})};

    return [willResize, custom, lock](const ResizeRequest& request)
    {
        auto width = (int) request.size.x;
        auto height = (int) request.size.y;
        willResize(width, height);

        auto size = custom({{(float) width, (float) height}, request.axis});
        return lock({size, request.axis});
    };
}

Point WindowOptions::effectiveInitialSize() const
{
    return effectiveSizeConstraint()(
        {{(float) width, (float) height}, ResizeAxis::Both});
}

Point WindowOptions::effectiveSize(Point size) const
{
    auto floored = Point {std::max(size.x, (float) minWidth),
                          std::max(size.y, (float) minHeight)};

    return effectiveSizeConstraint()({floored, ResizeAxis::Both});
}

Window::ContentViewLink::~ContentViewLink()
{
    attach(nullptr, nullptr);
}

void Window::ContentViewLink::attach(View* view, Window* window)
{
    if (contentView != nullptr)
        contentView->ownerWindow = nullptr;

    contentView = view;

    if (contentView != nullptr)
        contentView->ownerWindow = window;
}

Window::Window(View& view, const WindowOptions& optionsToUse)
    : Window(optionsToUse)
{
    setContentView(view);
}

void WindowInputListener::windowKeyEvent(const KeyEvent&) {}
void WindowInputListener::windowMouseEvent(const MouseEvent&) {}
void WindowInputListener::windowActivationChanged(bool) {}

void WindowInputTap::addListener(WindowInputListener& listener)
{
    listeners.addIfNotThere(&listener);
}

void WindowInputTap::removeListener(WindowInputListener& listener)
{
    auto match = [&listener](WindowInputListener* candidate)
    { return candidate == &listener; };

    std::erase_if(listeners.getVector(), match);
}

void WindowInputTap::keyEvent(const KeyEvent& event) const
{
    for (auto* listener: listeners)
        listener->windowKeyEvent(event);
}

void WindowInputTap::mouseEvent(const MouseEvent& event) const
{
    for (auto* listener: listeners)
        listener->windowMouseEvent(event);
}

void WindowInputTap::activationChanged(bool isKey)
{
    active = isKey;

    for (auto* listener: listeners)
        listener->windowActivationChanged(isKey);
}
} // namespace eacp::Graphics
