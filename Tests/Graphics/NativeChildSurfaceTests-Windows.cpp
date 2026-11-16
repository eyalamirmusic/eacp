#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/Graphics/Window/NativeChildSurface.h>

#include <optional>

// The surface's container is a child HWND, and a child window goes down with
// its top-level window. A host closes the window an editor was in and puts the
// same surface in the next one, and the handle the surface hands out there must
// be a live window in the new one: a plugin parented into the handle from
// before was parented into a destroyed window, at DPI 0 and size 0x0.
namespace NativeChildSurfaceTestsWindows
{
using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

WindowOptions surfaceWindowOptions()
{
    auto options = WindowOptions {};
    options.width = 300;
    options.height = 200;
    options.showInactive = true;
    return options;
}

void settleSurface()
{
    Threads::runEventLoopFor(Time::MS {100});
}

// WM_DPICHANGED_AFTERPARENT, which the header names only for WINVER 0x0605 and
// up.
constexpr auto dpiChangedAfterParent = UINT {0x02E3};

// A surface in a content view, put in one window that is then closed and
// destroyed, and then in a second.
struct Scene
{
    Scene()
    {
        content.addSubview(surface);
        surface.setBounds(surfaceBounds);
    }

    HWND handle() { return (HWND) surface.getNativeParentHandle(); }

    HWND moveToSecondWindow()
    {
        first.reset();
        settleSurface();

        second.emplace(content, surfaceWindowOptions());
        settleSurface();

        return handle();
    }

    HWND secondHost() { return (HWND) second->getHandle(); }

    // Where the container sits in its parent's client area, in pixels.
    RECT placementInParent(HWND container)
    {
        auto rect = RECT {};
        GetWindowRect(container, &rect);
        MapWindowPoints(nullptr, GetParent(container), (POINT*) &rect, 2);
        return rect;
    }

    RECT expectedPlacement(HWND host)
    {
        auto scale = static_cast<float>(GetDpiForWindow(host)) / 96.f;

        return {static_cast<LONG>(std::lround(surfaceBounds.x * scale)),
                static_cast<LONG>(std::lround(surfaceBounds.y * scale)),
                static_cast<LONG>(
                    std::lround((surfaceBounds.x + surfaceBounds.w) * scale)),
                static_cast<LONG>(
                    std::lround((surfaceBounds.y + surfaceBounds.h) * scale))};
    }

    Rect surfaceBounds {20.f, 10.f, 200.f, 150.f};
    View content;
    NativeChildSurface surface;
    std::optional<Window> first {std::in_place, content, surfaceWindowOptions()};
    std::optional<Window> second;
};

bool samePlacement(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right
           && a.bottom == b.bottom;
}

auto tAClosedWindowsContainerIsReplaced =
    test("NativeChildSurface/aClosedWindowsContainerIsReplacedInTheNextWindow") = []
{
    auto scene = Scene {};

    auto firstHandle = scene.handle();
    auto firstHost = (HWND) scene.first->getHandle();

    check(IsWindow(firstHandle) != FALSE);
    check(GetParent(firstHandle) == firstHost);

    scene.first.reset();
    settleSurface();

    check(IsWindow(firstHandle) == FALSE,
          "the container did not go down with its window");

    scene.second.emplace(scene.content, surfaceWindowOptions());
    settleSurface();

    auto handle = scene.handle();
    auto host = scene.secondHost();

    check(handle != nullptr);
    check(IsWindow(handle) != FALSE, "the surface handed out a destroyed window");
    check(GetParent(handle) == host, "the container is not in the second window");
    check(GetDpiForWindow(handle) == GetDpiForWindow(host));
    check(GetDpiForWindow(handle) != 0);
    check(same(scene.surface.backingScale(),
               static_cast<float>(GetDpiForWindow(host)) / 96.f));
    check(samePlacement(scene.placementInParent(handle),
                        scene.expectedPlacement(host)),
          "the new container does not cover the surface's bounds");
};

// The new container is wired to the surface the way the first one was: the
// DPI notification places it again, and a plain key it is handed reaches
// onUnhandledKey.
auto tTheNewContainerIsWired =
    test("NativeChildSurface/theReplacementContainerIsWiredToTheSurface") = []
{
    auto scene = Scene {};
    scene.handle();

    auto handle = scene.moveToSecondWindow();
    auto host = scene.secondHost();

    check(IsWindow(handle) != FALSE);

    SetWindowPos(handle,
                 nullptr,
                 0,
                 0,
                 10,
                 10,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    SendMessageW(handle, dpiChangedAfterParent, 0, 0);

    check(samePlacement(scene.placementInParent(handle),
                        scene.expectedPlacement(host)),
          "the DPI notification did not reach the surface");

    auto keys = 0;
    scene.surface.onUnhandledKey = [&](const NativeKeyEvent&)
    {
        ++keys;
        return true;
    };

    PostMessageW(handle, WM_KEYDOWN, 'A', 0x00000001);
    settleSurface();

    check(keys == 1, "a key the container was handed did not reach the surface");
};

// Between the windows the surface has no container: it hands out nothing, and
// hiding or resizing it touches no dead handle. Hidden then, the container it
// gets in the next window comes up hidden.
auto tADeadContainerIsLeftAlone =
    test("NativeChildSurface/aSurfaceBetweenWindowsHasNoContainer") = []
{
    auto scene = Scene {};

    check(IsWindow(scene.handle()) != FALSE);

    scene.first.reset();
    settleSurface();

    scene.surface.setVisible(false);
    scene.surface.setBounds({0.f, 0.f, 50.f, 50.f});

    check(scene.surface.getNativeParentHandle() == nullptr);

    scene.second.emplace(scene.content, surfaceWindowOptions());
    settleSurface();

    auto handle = scene.handle();

    check(IsWindow(handle) != FALSE);
    check(IsWindowVisible(handle) == FALSE,
          "a hidden surface's new container is visible");
};
} // namespace NativeChildSurfaceTestsWindows
