#include <eacp/Core/Utils/WinInclude.h>

#include "NativeChildSurface.h"
#include "CompositionHostWindow-Windows.h"
#include "WindowGeometry-Windows.h"

#include <eacp/Core/Plugins/ModuleInfo.h>

// The first child HWND eacp puts inside one of its own windows, and the one
// place it has to: a foreign toolkit is given an HWND to parent into or it is
// given nothing, and a View's handle is an IDCompositionVisual2.
//
// Which settles the z-order question before it is asked. DWM composites a
// child window over the parent's DirectComposition tree, so the foreign
// content draws above every eacp view in the same window whatever the View
// tree's own order says — WebView-Windows.cpp says as much where it explains
// why it renders into a visual instead ("impossible with a child HWND"). That
// is the right way round for what this is for, a hosted plugin's editor filling
// the rectangle a host gave it, and it is a real constraint on anything else:
// eacp content cannot be drawn over this surface, only around it. A host that
// needs an overlay has to hide the surface rather than cover it — and hiding is
// ShowWindow, not the visual tree, which is why visibilityChanged is hooked.
//
// The same goes for the mouse: the rectangle belongs to the child window and
// the View underneath it never sees a click, which is exactly what a hosted
// editor wants and is worth knowing before putting a widget behind one.

namespace eacp::Graphics
{

namespace
{
// This module's own name. Window classes live in a process-global registry, so
// a host and a plugin that each carry a copy of eacp must not register the same
// literal — and the name is distinct from EmbeddedView's both for that reason
// and because a unity build puts the two files in one translation unit.
const auto nativeChildClassNameStorage =
    Plugins::getUniqueWindowClassName(L"EACPNativeChildSurfaceClass");
const wchar_t* nativeChildClassName = nativeChildClassNameStorage.c_str();
bool nativeChildClassRegistered = false;

LRESULT CALLBACK nativeChildWindowProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wParam,
                                       LPARAM lParam)
{
    // The container draws nothing and decides nothing: every pixel of it is
    // covered by the foreign child, and every message that matters is that
    // child's. Erasing is claimed rather than defaulted so the moment between
    // the window existing and the plugin parenting into it is not a flash of
    // whatever brush the class was given.
    if (msg == WM_ERASEBKGND)
        return 1;

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void registerNativeChildClass()
{
    if (nativeChildClassRegistered)
        return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = nativeChildWindowProc;
    wc.hInstance = (HINSTANCE) Plugins::getCurrentModuleHandle();

    // No cursor of our own: the foreign child sets the pointer over its own
    // window, and a class cursor here would only show in the instant before it
    // is parented.
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = nativeChildClassName;

    nativeChildClassRegistered = RegisterClassExW(&wc) != 0;
}

// Where a view's top-left sits in its host window's client area, in points.
//
// Accumulated up the parent chain because this is the one thing the Windows
// View tree does not already know. A view's DComp visual is offset in its
// parent visual's space and DComp composes the chain, so nothing ever needed
// the total — a child window is placed in its window's coordinates and needs
// nothing else. The root's bounds are {0, 0, w, h}, so walking through it
// costs nothing.
Point originInHostWindow(View& view)
{
    auto origin = Point(0.f, 0.f);

    for (auto* current = &view; current != nullptr; current = current->getParent())
    {
        auto bounds = current->getBounds();
        origin.x += bounds.x;
        origin.y += bounds.y;
    }

    return origin;
}

// Whether the view is shown at all, its ancestors included. View::setVisible
// reports every later change through visibilityChanged, but a surface built
// under an already-hidden ancestor has been told nothing and would otherwise
// come up as a visible child window of a window whose eacp content is not
// there.
bool isEffectivelyVisible(View& view)
{
    for (auto* current = &view; current != nullptr; current = current->getParent())
        if (!current->isVisible())
            return false;

    return true;
}
} // namespace

struct NativeChildSurface::Native
{
    explicit Native(View& ownerToUse)
        : owner(&ownerToUse)
    {
    }

    ~Native() { destroyWindow(); }

    // Built on demand rather than in the constructor. A child window needs a
    // parent window to be created against, and a View has none until it is in
    // a Window or an EmbeddedView — later than its own construction, and no
    // earlier than the moment a plugin format asks for the handle.
    //
    // findHostHwndForView rather than View::getWindow()->getHandle(): the
    // second is null inside an EmbeddedView, which is every plugin editor —
    // the exact case this class exists for. The registry behind it is filled
    // by CompositionHostWindow, which both surfaces go through.
    //
    // Threads::attachCurrentThreadAsMain is not called here for the same
    // reason: there is no host HWND until a Window or an EmbeddedView made one,
    // and making one is what attaches the thread.
    HWND ensureWindow()
    {
        auto host = findHostHwndForView(owner);

        if (host == nullptr)
            return nullptr;

        if (hwnd == nullptr)
            createWindow(host);
        else if (GetParent(hwnd) != host)
            SetParent(hwnd, host); // The view was moved to another window.

        place();

        return hwnd;
    }

    void createWindow(HWND host)
    {
        registerNativeChildClass();

        // WS_CLIPCHILDREN so the foreign child owns its pixels rather than
        // being painted over, WS_CLIPSIBLINGS so two surfaces in one window do
        // not fight. Nothing else and no extended style: a plain child window
        // is what every plugin format means by an HWND to parent into.
        auto style = DWORD {WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN};

        if (isEffectivelyVisible(*owner))
            style |= WS_VISIBLE;

        // Placed at nothing and sized by place() immediately below, so there is
        // one piece of code deciding where the surface goes.
        hwnd = CreateWindowExW(0,
                               nativeChildClassName,
                               L"",
                               style,
                               0,
                               0,
                               0,
                               0,
                               host,
                               nullptr,
                               (HINSTANCE) Plugins::getCurrentModuleHandle(),
                               nullptr);
    }

    void place()
    {
        if (hwnd == nullptr)
            return;

        auto size = owner->getLocalBounds();
        auto origin = originInHostWindow(*owner);
        auto pixels = detail::toPhysicalPixels(
            Rect {origin.x, origin.y, size.w, size.h}, dpiScale());

        SetWindowPos(hwnd,
                     nullptr,
                     pixels.left,
                     pixels.top,
                     pixels.right - pixels.left,
                     pixels.bottom - pixels.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }

    // The window's own DPI, which is what View's paint surfaces and the WebView
    // both measure against. Not EmbeddedView::setPixelsPerPoint's override: a
    // View has no way to reach the CompositionHostWindow holding it, so a host
    // doing its own scaling is a gap here in the same way and to the same
    // extent it is for every other view in the tree.
    float dpiScale() const
    {
        if (auto host = findHostHwndForView(owner))
            return static_cast<float>(GetDpiForWindow(host)) / 96.f;

        return static_cast<float>(GetDpiForSystem()) / 96.f;
    }

    void setVisible(bool shouldBeVisible)
    {
        if (hwnd == nullptr)
            return;

        // SW_SHOWNA, not SW_SHOW: a surface coming back must not take the focus
        // off whatever in the window had it.
        ShowWindow(hwnd, shouldBeVisible ? SW_SHOWNA : SW_HIDE);
    }

    // Takes the container down and nothing else.
    //
    // DestroyWindow destroys a window's children with it, and the children here
    // are not ours to destroy — a plugin that is still attached would have its
    // editor pulled out from under it and would then destroy it a second time.
    // Anything still parented in is therefore let go of first. A well-behaved
    // host has already detached it (a VST3 plugin is sent removed()), so this
    // is a backstop rather than the normal path.
    void destroyWindow()
    {
        if (hwnd == nullptr)
            return;

        while (auto child = GetWindow(hwnd, GW_CHILD))
        {
            ShowWindow(child, SW_HIDE);

            // Returns the old parent, so null means it could not be detached
            // and the loop would not otherwise end.
            if (SetParent(child, nullptr) == nullptr)
                break;
        }

        DestroyWindow(hwnd);
        hwnd = nullptr;
    }

    View* owner {};
    HWND hwnd {};
};

NativeChildSurface::NativeChildSurface()
    : impl(*this)
{
}

NativeChildSurface::~NativeChildSurface() = default;

void* NativeChildSurface::getNativeParentHandle()
{
    return impl->ensureWindow();
}

void NativeChildSurface::refreshPlacement()
{
    impl->ensureWindow();
}

void NativeChildSurface::resized()
{
    View::resized();
    refreshPlacement();
}

// Hiding the view hides its visual, and the container is not in that tree: it
// is a live child window of the same HWND, still drawing and still taking the
// mouse over its rectangle. So it is told separately.
void NativeChildSurface::visibilityChanged(bool nowVisible)
{
    impl->setVisible(nowVisible);
}

} // namespace eacp::Graphics
