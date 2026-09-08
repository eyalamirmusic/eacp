#include "Window.h"

#include "LinuxWindowNative-Linux.h"
#include "LinuxWindowSystem-Linux.h"
#include "WaylandWindow-Linux.h"
#include "X11Connection-Linux.h"
#include "X11Window-Linux.h"

namespace eacp::Graphics
{
namespace
{
std::unique_ptr<LinuxWindowNative> makeWindowNative(const WindowOptions& options,
                                                    WindowEvents& events)
{
    switch (linuxPreferredWindowSystem())
    {
        case LinuxWindowSystem::Wayland:
            return makeWaylandWindowNative(options, events);

        // A server that could not be reached leaves the same surfaceless
        // window an unreachable compositor does.
        case LinuxWindowSystem::X11:
            if (x11Connection() != nullptr)
                return makeX11WindowNative(options, events);

            break;

        case LinuxWindowSystem::None:
            break;
    }

    return makeHeadlessWindowNative(options, events);
}
} // namespace

struct Window::Native
{
    Native(const WindowOptions& options, WindowEvents& events)
        : native(makeWindowNative(options, events))
    {
    }

    std::unique_ptr<LinuxWindowNative> native;
};

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(optionsToUse, events)
{
}

Window::~Window() = default;

void Window::setTitle(const std::string& title)
{
    impl->native->setTitle(title);
}

// The wl_surface on Wayland; on X11 the xcb_window_t widened into a pointer,
// which is the form every plugin API hands an X11 id out in.
void* Window::getHandle()
{
    return impl->native->getHandle();
}

void* Window::getContentViewHandle()
{
    return impl->native->getContentViewHandle();
}

void Window::setContentView(View& view)
{
    contentLink.attach(&view, this);
    impl->native->setContentView(&view);
}

// Only shows the window: a Wayland client cannot raise itself at all, and
// raising an X11 one is the window manager's to grant, not ours to take.
void Window::toFront()
{
    impl->native->setVisible(true);
}

void Window::setVisible(bool visible)
{
    impl->native->setVisible(visible);
}

void Window::minimize()
{
    impl->native->minimize();
}

void Window::toggleMaximize()
{
    impl->native->toggleMaximize();
}

bool Window::isVisible()
{
    return impl->native->isVisible();
}

Point Window::getPosition() const
{
    return impl->native->getPosition();
}

void Window::setPosition(Point position)
{
    impl->native->setPosition(position);
}

void Window::setMouseLocked(bool locked)
{
    impl->native->setMouseLocked(locked);
}

bool Window::isMouseLocked() const
{
    return impl->native->isMouseLocked();
}

// The native unit here is the evdev keycode.
bool Window::isKeyPressed(uint16_t nativeKeyCode) const
{
    return impl->native->isKeyPressed(nativeKeyCode);
}

bool Window::isShiftPressed() const
{
    return getModifiers().shift;
}

bool Window::isControlPressed() const
{
    return getModifiers().control;
}

bool Window::isAltPressed() const
{
    return getModifiers().alt;
}

bool Window::isCommandPressed() const
{
    return getModifiers().command;
}

ModifierKeys Window::getModifiers() const
{
    return impl->native->getModifiers();
}

} // namespace eacp::Graphics
