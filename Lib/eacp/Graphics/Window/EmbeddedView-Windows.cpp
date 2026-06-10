#include <eacp/Core/Utils/WinInclude.h>

#include "EmbeddedView.h"
#include "CompositionHostWindow-Windows.h"

namespace eacp::Graphics
{

static const wchar_t* EMBEDDED_CLASS_NAME = L"EACPEmbeddedViewClass";
static bool embeddedClassRegistered = false;

struct EmbeddedView::Native
{
    Native(void* hostParentHandle, const EmbeddedViewOptions& options)
    {
        registerWindowClass();
        createChildWindow((HWND) hostParentHandle, options);
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
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = EMBEDDED_CLASS_NAME;

        RegisterClassExW(&wc);
        embeddedClassRegistered = true;
    }

    void createChildWindow(HWND parent, const EmbeddedViewOptions& options)
    {
        auto dpi = parent ? GetDpiForWindow(parent) : GetDpiForSystem();
        auto dpiScale = static_cast<float>(dpi) / 96.f;
        auto physicalWidth = static_cast<int>(options.width * dpiScale);
        auto physicalHeight = static_cast<int>(options.height * dpiScale);

        DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

        host.hwnd = CreateWindowExW(0,
                                    EMBEDDED_CLASS_NAME,
                                    L"",
                                    style,
                                    0,
                                    0,
                                    physicalWidth,
                                    physicalHeight,
                                    parent,
                                    nullptr,
                                    GetModuleHandleW(nullptr),
                                    this);
    }

    void setSize(int width, int height)
    {
        if (!host.hwnd)
            return;

        auto dpiScale = host.getDpiScale();
        auto physicalWidth = static_cast<int>(width * dpiScale);
        auto physicalHeight = static_cast<int>(height * dpiScale);

        SetWindowPos(host.hwnd,
                     nullptr,
                     0,
                     0,
                     physicalWidth,
                     physicalHeight,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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
    : options(optionsToUse)
    , impl(hostParentHandle, options)
{
}

EmbeddedView::~EmbeddedView() = default;

void EmbeddedView::setContentView(View& view)
{
    impl->host.attachContentView(&view);
}

void EmbeddedView::setSize(int width, int height)
{
    impl->setSize(width, height);
}

void* EmbeddedView::getHandle()
{
    return impl->host.hwnd;
}

} // namespace eacp::Graphics
