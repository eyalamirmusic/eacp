#include "Window.h"

#include "LinuxWindowSystem-Linux.h"
#include "WaylandDisplay-Linux.h"
#include "X11Connection-Linux.h"

// Docking one window to another here: WM_TRANSIENT_FOR on X11,
// xdg_toplevel_set_parent on Wayland, and nothing at all under a window with
// no window system beneath it. Both backends are in every copy, so which one
// a docking speaks is the same runtime decision the window itself was.
//
// Neither of them places the child. An X11 transient is placed by the window
// manager and a Wayland client has no positions at all, so what keeps the
// offset is the portable carrier — and on Wayland the position it carries is
// one only eacp can see (LinuxWindowNative::setPosition hands back the value
// it was given), which is honest but invisible to the compositor.

namespace eacp::Graphics::detail
{
namespace
{
// A Window's handle is a wl_surface on one window system and an id on the
// other, and each connection already keeps the map from one to the toplevel
// behind it - so the docking is asked of the same object every other piece of
// glue starts from. Null for a window with no window system under it: a
// headless one, or one whose server never answered.
WaylandWindowSurface* waylandToplevel(Window& window)
{
    auto* display = waylandDisplay();
    auto* handle = window.getHandle();

    if (display == nullptr || handle == nullptr)
        return nullptr;

    return display->findSurface(static_cast<wl_surface*>(handle)).window;
}

X11WindowSurface* x11Toplevel(Window& window)
{
    auto* connection = x11Connection();
    auto* handle = window.getHandle();

    if (connection == nullptr || handle == nullptr)
        return nullptr;

    const auto id = (xcb_window_t) (uintptr_t) handle;

    return connection->findWindow(id).windowSurface;
}
} // namespace

// The window system each native really is, taken from the same preference
// Window-Linux.cpp built it from: a transient on X11 is WM_TRANSIENT_FOR, on
// Wayland it is xdg_toplevel_set_parent, and a headless window has neither to
// say it to. Both are idempotent, which is what lets this be called on every
// dock, show and hide.
void attachNativeParent(Window& child, Window* parent)
{
    switch (linuxPreferredWindowSystem())
    {
        case LinuxWindowSystem::Wayland:
            if (auto* toplevel = waylandToplevel(child))
                toplevel->setWindowParent(
                    parent != nullptr ? waylandToplevel(*parent) : nullptr);

            break;

        case LinuxWindowSystem::X11:
            if (auto* toplevel = x11Toplevel(child))
                toplevel->setWindowParent(parent != nullptr ? x11Toplevel(*parent)
                                                            : nullptr);

            break;

        case LinuxWindowSystem::None:
            break;
    }
}

// A transient window is stacked above its parent and minimised with it by the
// window manager, but neither protocol moves it when the parent moves, so the
// portable carrier is the one that keeps the offset. Wayland has no window
// positions at all, which is the other reason this stays false: there the
// carrier moves a position that only eacp can see, and the compositor places
// the child itself.
bool backendCarriesChildWindows()
{
    return false;
}
} // namespace eacp::Graphics::detail
