#include <eacp/Core/Utils/WinInclude.h>

#include "Window.h"

namespace eacp::Graphics::detail
{
namespace
{
HWND nativeWindow(Window& window)
{
    return static_cast<HWND>(window.getHandle());
}
} // namespace

// A top-level window's GWLP_HWNDPARENT is its owner, and an owned window is
// what a docked one is on Win32: it stays above its owner and above nothing
// else, is hidden while the owner is minimised and shown again with it, and is
// destroyed with it — which is why the portable layer detaches every child
// before a parent's native window goes away.
//
// The taskbar button an owned window must not have is the shell's decision,
// taken the first time the window is shown. A Window is created hidden and
// nothing here adds WS_EX_APPWINDOW or WS_EX_TOOLWINDOW, so a child docked
// through WindowOptions::parent — which happens in the constructor, before
// anything shows it — never gets a button to take away. One docked later,
// while it is already on screen, keeps the button the shell gave it until it
// is hidden and shown again.
void attachNativeParent(Window& child, Window* parent)
{
    auto* childWindow = nativeWindow(child);

    if (childWindow == nullptr)
        return;

    auto* ownerWindow = parent != nullptr ? nativeWindow(*parent) : nullptr;

    if (GetWindow(childWindow, GW_OWNER) == ownerWindow)
        return;

    SetWindowLongPtrW(
        childWindow, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerWindow));

    // Ownership is what the two windows are ordered by from now on, not a
    // restacking of where they are: a child docked while both are already on
    // screen has to be lifted above its new owner once by hand.
    if (ownerWindow != nullptr)
        SetWindowPos(childWindow,
                     HWND_TOP,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Owned windows travel with nothing: Win32 hides them when the owner is
// minimised and destroys them with it, but a move of the owner moves only the
// owner, so the portable carrier is the one that keeps the offset.
bool backendCarriesChildWindows()
{
    return false;
}
} // namespace eacp::Graphics::detail
