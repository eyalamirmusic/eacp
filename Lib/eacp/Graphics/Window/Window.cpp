#include "Window.h"

namespace eacp::Graphics
{
Window::Window(View& view, const WindowOptions& optionsToUse)
    : Window(optionsToUse)
{
    setContentView(view);
}

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
