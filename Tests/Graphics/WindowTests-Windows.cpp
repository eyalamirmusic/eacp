#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>

// Docked windows, Win32's half. WindowTests.cpp covers the bookkeeping and the
// lifetime; what only shows up here is that the link reached the HWND at all —
// a child that is registered with its parent and never given it as an owner
// keeps none of the ordering, is not hidden when the parent is minimised and
// is not destroyed with it, and every portable case still passes, because the
// portable carrier would move it anyway.
//
// The last one is the reason the detach cannot wait for the backend's
// destructor: DestroyWindow takes the windows it owns with it, so a child that
// still names a dying parent is a child that dies with it.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
HWND nativeWindow(Window& window)
{
    return static_cast<HWND>(window.getHandle());
}

HWND ownerOf(Window& window)
{
    return GetWindow(nativeWindow(window), GW_OWNER);
}

WindowOptions dockedChildOptions(Window& parent)
{
    auto options = WindowOptions {};
    options.parent = &parent;
    return options;
}
} // namespace

auto tChildIsAnOwnedWindow = test("Window/childBecomesAnOwnedWindow") = []
{
    auto parent = Window {};
    auto child = Window {dockedChildOptions(parent)};

    check(ownerOf(child) == nativeWindow(parent));
    check(ownerOf(parent) == nullptr);
};

auto tUndockingClearsTheOwner = test("Window/undockingClearsTheWin32Owner") = []
{
    auto parent = Window {};
    auto child = Window {dockedChildOptions(parent)};

    child.setParent(nullptr);

    check(ownerOf(child) == nullptr);
};

// A child hidden while Win32 still holds the link is shown again the next time
// its owner is restored from the taskbar, so hiding one has to drop the owner
// — and showing it has to put it back, or the palette comes back undocked.
auto tHiddenChildLeavesItsOwner = test("Window/hiddenChildLeavesItsWin32Owner") = []
{
    auto parent = Window {};
    auto child = Window {dockedChildOptions(parent)};

    child.setVisible(false);

    check(ownerOf(child) == nullptr);
    check(child.getParent() == &parent);

    child.setVisible(true);

    check(ownerOf(child) == nativeWindow(parent));
};

// The destruction order that would otherwise take the child down with the
// parent: the owner goes before the parent's HWND does, so what is left is an
// ordinary top-level window rather than a destroyed one.
auto tDestroyedParentLeavesTheChildWindowAlive =
    test("Window/destroyedParentLeavesTheChildWindowAlive") = []
{
    auto child = std::optional<Window> {};

    {
        auto parent = Window {};
        child.emplace(dockedChildOptions(parent));
        check(ownerOf(*child) == nativeWindow(parent));
    }

    check(IsWindow(nativeWindow(*child)) != FALSE);
    check(ownerOf(*child) == nullptr);
};
