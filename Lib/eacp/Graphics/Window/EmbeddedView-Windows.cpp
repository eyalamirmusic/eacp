#include <eacp/Core/Utils/WinInclude.h>

#include "EmbeddedView.h"
#include "CompositionHostWindow-Windows.h"
#include "WindowGeometry-Windows.h"

namespace eacp::Graphics
{

static const std::wstring EMBEDDED_CLASS_NAME_STORAGE =
    eacp::Plugins::getUniqueWindowClassName(L"EACPEmbeddedViewClass");
static const wchar_t* EMBEDDED_CLASS_NAME = EMBEDDED_CLASS_NAME_STORAGE.c_str();
static bool embeddedClassRegistered = false;

struct EmbeddedView::Native
{
    Native(void* hostParentHandle, const Rect& initialBounds)
    {
        Threads::attachCurrentThreadAsMain();
        registerWindowClass();
        createChildWindow((HWND) hostParentHandle, initialBounds);
        host.initializeComposition(false);
    }

    ~Native() { host.teardown(); }

    static void registerWindowClass()
    {
        if (embeddedClassRegistered)
            return;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = (HINSTANCE) eacp::Plugins::getCurrentModuleHandle();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = EMBEDDED_CLASS_NAME;

        embeddedClassRegistered = RegisterClassExW(&wc) != 0;
    }

    void createChildWindow(HWND parent, const Rect& initialBounds)
    {
        // The parent's DPI, not our own: there is no window of ours to ask yet.
        auto dpi = parent ? GetDpiForWindow(parent) : GetDpiForSystem();
        auto pixels =
            detail::toPhysicalPixels(initialBounds, static_cast<float>(dpi) / 96.f);

        DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

        host.hwnd =
            CreateWindowExW(0,
                            EMBEDDED_CLASS_NAME,
                            L"",
                            style,
                            pixels.left,
                            pixels.top,
                            pixels.right - pixels.left,
                            pixels.bottom - pixels.top,
                            parent,
                            nullptr,
                            (HINSTANCE) eacp::Plugins::getCurrentModuleHandle(),
                            this);
    }

    void setBounds(const Rect& bounds)
    {
        if (!host.hwnd)
            return;

        auto pixels = detail::toPhysicalPixels(bounds, host.getDpiScale());

        SetWindowPos(host.hwnd,
                     nullptr,
                     pixels.left,
                     pixels.top,
                     pixels.right - pixels.left,
                     pixels.bottom - pixels.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }

    void stopFollowingHost()
    {
        // Nothing to undo. A child window is moved and sized by whoever calls
        // SetWindowPos on it and by nobody else, so unlike AppKit's
        // autoresizing there is no second party here to switch off.
    }

    void setVisible(bool shouldBeVisible)
    {
        if (!host.hwnd)
            return;

        // SW_SHOWNA, not SW_SHOW: bringing the surface back must not take the
        // focus off whatever in the host's window had it.
        ShowWindow(host.hwnd, shouldBeVisible ? SW_SHOWNA : SW_HIDE);
    }

    static LRESULT CALLBACK windowProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wParam,
                                       LPARAM lParam);

    CompositionHostWindow host;
};

LRESULT CALLBACK EmbeddedView::Native::windowProc(HWND hwnd,
                                                  UINT msg,
                                                  WPARAM wParam,
                                                  LPARAM lParam)
{
    Native* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Native*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->host.hwnd = hwnd;
    }
    else
    {
        self = reinterpret_cast<Native*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (auto result = self->host.handleCommonMessage(msg, wParam, lParam))
        return *result;

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

EmbeddedView::EmbeddedView(void* hostParentHandle,
                           const EmbeddedViewOptions& optionsToUse)
    : bounds(0.f, 0.f, (float) optionsToUse.width, (float) optionsToUse.height)
    , impl(hostParentHandle, bounds)
{
}

EmbeddedView::~EmbeddedView() = default;

void EmbeddedView::setContentView(View& view)
{
    impl->host.attachContentView(&view);
}

void EmbeddedView::setBounds(const Rect& newBounds)
{
    bounds = newBounds;

    impl->stopFollowingHost();
    impl->setBounds(bounds);
}

void EmbeddedView::setSize(int width, int height)
{
    bounds = bounds.withSize((float) width, (float) height);
    impl->setBounds(bounds);
}

void EmbeddedView::setVisible(bool shouldBeVisible)
{
    visible = shouldBeVisible;
    impl->setVisible(shouldBeVisible);
}

void EmbeddedView::setPixelsPerPoint(float pixelsPerPoint)
{
    impl->host.setDpiScaleOverride(pixelsPerPoint);

    // The frame was measured against the answer that has just changed.
    impl->setBounds(bounds);
}

void* EmbeddedView::getHandle()
{
    return impl->host.hwnd;
}

} // namespace eacp::Graphics
