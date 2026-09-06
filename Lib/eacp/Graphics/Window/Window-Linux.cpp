#include "Window.h"

// The Linux Window, with no window system under it.
//
// Windows already has this mode: Apps::getAppEnvironment().headless makes it
// build the HWND and the visual tree and simply never show them, so a CI runner
// with no session still runs the whole suite. On Linux there is nothing to
// suppress — the Wayland surface, xdg_toplevel and libdecor frame are plan
// stage 4 — so a Window is always in that mode, and this file is what the
// contract Window.h documents for headless looks like when it is the only mode
// there is.
//
// Which means: the geometry, the title, the flags and the mouse-lock intent are
// real and answer honestly, the content view is adopted and sized exactly as it
// is on the other platforms, and every gesture that would put something on a
// screen — toFront, setVisible, minimize, toggleMaximize — is a no-op that
// records nothing, because there is no state behind it to record.
//
// Deliberately unlike Windows in one respect: constructing a Window there
// forces the whole 2D stack up (a D2D factory, a composition device). Nothing
// here touches drawing at all, which is what lets eacp-graphics link on a Linux
// box with no GPU and no display libraries installed.

namespace eacp::Graphics
{

struct Window::Native
{
    Native(const WindowOptions& optionsToUse, WindowEvents& eventsToUse)
        : title(optionsToUse.title)
        , contentSize {(float) optionsToUse.width, (float) optionsToUse.height}
        , events(&eventsToUse)
    {
        if (optionsToUse.initialPosition)
            position = *optionsToUse.initialPosition;
    }

    void setContentView(View* view)
    {
        contentView = view;

        if (contentView != nullptr)
            contentView->setBounds({0.f, 0.f, contentSize.x, contentSize.y});
    }

    // A window has a frame whether or not it is on screen (Window.h), so this
    // half of the contract is kept: what setPosition and initialPosition put in
    // is what getPosition hands back, and a move is reported as a move however
    // it was made.
    void setPosition(Point newPosition)
    {
        position = newPosition;
        events->onMoved(position);
    }

    std::string title;
    Point contentSize;
    WindowEvents* events;

    Point position;
    View* contentView = nullptr;
    bool mouseLocked = false;
};

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(optionsToUse, events)
{
}

Window::~Window() = default;

void Window::setTitle(const std::string& title)
{
    impl->title = title;
}

// No surface, so there is no handle to give out and nothing for a foreign host
// to parent into. Stage 4 makes the first of these the wl_surface.
void* Window::getHandle()
{
    return nullptr;
}

void* Window::getContentViewHandle()
{
    return nullptr;
}

void Window::setContentView(View& view)
{
    contentLink.attach(&view, this);
    impl->setContentView(&view);
}

// Nothing to order in, out or front of. Kept callable so portable app code
// stays unconditional, per the headless contract in Window.h.
void Window::toFront() {}
void Window::setVisible(bool) {}
void Window::minimize() {}
void Window::toggleMaximize() {}

bool Window::isVisible()
{
    return false;
}

Point Window::getPosition() const
{
    return impl->position;
}

void Window::setPosition(Point position)
{
    impl->setPosition(position);
}

// Intent, as everywhere else: the lock engages while the window has key focus,
// and this one never does. Remembered so an app can toggle and read it back.
void Window::setMouseLocked(bool locked)
{
    impl->mouseLocked = locked;
}

bool Window::isMouseLocked() const
{
    return impl->mouseLocked;
}

// No keyboard reaches a window with no surface. The real answers come from an
// xkb state fed by wl_keyboard (plan stage 4); until then nothing is pressed,
// which is what a window that cannot be focused should say.
bool Window::isKeyPressed(uint16_t) const
{
    return false;
}

bool Window::isShiftPressed() const
{
    return false;
}

bool Window::isControlPressed() const
{
    return false;
}

bool Window::isAltPressed() const
{
    return false;
}

bool Window::isCommandPressed() const
{
    return false;
}

ModifierKeys Window::getModifiers() const
{
    return {};
}

} // namespace eacp::Graphics
