#include <eacp/Graphics/Window/Window.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp::Graphics;

// Mouse lock is intent-based: setMouseLocked records the desired state and
// the OS-level lock only engages while the window has key focus, so the
// intent must be togglable (and readable) on a window that never becomes
// key, like here.
auto tMouseLockIntent = test("Window/mouseLockIntentToggles") = []
{
    auto window = Window {};

    check(!window.isMouseLocked());

    window.setMouseLocked(true);
    check(window.isMouseLocked());

    window.setMouseLocked(true);
    check(window.isMouseLocked());

    window.setMouseLocked(false);
    check(!window.isMouseLocked());
};
