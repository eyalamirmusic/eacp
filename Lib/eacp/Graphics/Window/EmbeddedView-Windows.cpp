#include "EmbeddedView.h"
#include "../Layers/NativeLayer-Windows.h"

#include <bitset>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.Display.h>
#include <Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>

namespace VKEmbedded
{
constexpr int Shift = 0x10;
constexpr int Control = 0x11;
constexpr int Menu = 0x12;
constexpr int LWin = 0x5B;
constexpr int RWin = 0x5C;
} // namespace VKEmbedded

inline int getXFromLParamEmbedded(LPARAM lp)
{
    return static_cast<int>(static_cast<short>(LOWORD(lp)));
}
inline int getYFromLParamEmbedded(LPARAM lp)
{
    return static_cast<int>(static_cast<short>(HIWORD(lp)));
}

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

wuc::Compositor getWinRTCompositor();

static const wchar_t* EMBEDDED_CLASS_NAME = L"EACPEmbeddedViewClass";
static bool embeddedClassRegistered = false;

struct EmbeddedView::Native
{
    Native(void* hostParentHandle, const EmbeddedViewOptions& options)
    {
        keyState.reset();
        registerWindowClass();
        createChildWindow((HWND) hostParentHandle, options);
        initializeComposition();
    }

    ~Native()
    {
        if (rootVisual)
            rootVisual.Children().RemoveAll();

        rootVisual = nullptr;
        target = nullptr;

        if (hwnd)
            DestroyWindow(hwnd);
    }

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
        auto dpi = GetDpiForSystem();
        auto dpiScale = static_cast<float>(dpi) / 96.f;
        auto physicalWidth = static_cast<int>(options.width * dpiScale);
        auto physicalHeight = static_cast<int>(options.height * dpiScale);

        DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

        hwnd = CreateWindowExW(0,
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

    void initializeComposition()
    {
        if (!hwnd)
            return;

        auto compositor = getWinRTCompositor();
        if (!compositor)
            return;

        auto interop = compositor.as<
            ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        winrt::com_ptr<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget>
            abiTarget;
        auto hr = interop->CreateDesktopWindowTarget(hwnd, 0, abiTarget.put());
        if (FAILED(hr) || !abiTarget)
            return;

        target = abiTarget.as<wuc::Desktop::DesktopWindowTarget>();
        rootVisual = compositor.CreateContainerVisual();

        auto dpiScale = getDpiScale();
        rootVisual.Scale({dpiScale, dpiScale, 1.0f});

        target.Root(rootVisual);
    }

    float getDpiScale() const
    {
        auto dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
        return dpi / 96.f;
    }

    void setContentView(View* view)
    {
        contentView = view;

        if (!hwnd || !view)
            return;

        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        auto dpiScale = getDpiScale();
        view->setBounds({0.f,
                         0.f,
                         (float) clientRect.right / dpiScale,
                         (float) clientRect.bottom / dpiScale});

        auto* viewVisual = static_cast<wuc::ContainerVisual*>(view->getHandle());

        if (rootVisual && viewVisual)
            rootVisual.Children().InsertAtTop(*viewVisual);

        ensureAllLayersRendered(view);
    }

    void setSize(int width, int height)
    {
        if (!hwnd)
            return;

        auto dpiScale = getDpiScale();
        auto physicalWidth = static_cast<int>(width * dpiScale);
        auto physicalHeight = static_cast<int>(height * dpiScale);

        SetWindowPos(hwnd,
                     nullptr,
                     0,
                     0,
                     physicalWidth,
                     physicalHeight,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void ensureAllLayersRendered(const View* view)
    {
        if (!view)
            return;

        for (auto* layer: view->getLayers())
        {
            auto* native = static_cast<NativeLayerBase*>(layer->getNativeLayer());
            if (native)
                native->ensureContent();
        }

        for (auto* subview: view->getSubviews())
            ensureAllLayersRendered(subview);
    }

    void onKeyDown(uint16_t vk)
    {
        if (vk < 256)
            keyState.set(vk);
    }

    void onKeyUp(uint16_t vk)
    {
        if (vk < 256)
            keyState.reset(vk);
    }

    ModifierKeys getModifiers() const
    {
        auto pressed = [&](int vk) { return vk < 256 && keyState.test(vk); };
        return {pressed(VKEmbedded::Shift),
                pressed(VKEmbedded::Control),
                pressed(VKEmbedded::Menu),
                pressed(VKEmbedded::LWin) || pressed(VKEmbedded::RWin)};
    }

    static LRESULT CALLBACK windowProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wParam,
                                       LPARAM lParam);

    HWND hwnd = nullptr;
    View* contentView = nullptr;
    wuc::Desktop::DesktopWindowTarget target {nullptr};
    wuc::ContainerVisual rootVisual {nullptr};
    std::bitset<256> keyState;
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
        self->hwnd = hwnd;
    }
    else
    {
        self = reinterpret_cast<Native*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg)
    {
        case WM_SIZE:
            if (self->contentView)
            {
                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                auto dpiScale = self->getDpiScale();
                self->contentView->setBounds(
                    Rect(0,
                         0,
                         static_cast<float>(clientRect.right) / dpiScale,
                         static_cast<float>(clientRect.bottom) / dpiScale));
                self->ensureAllLayersRendered(self->contentView);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            if (self->contentView)
                self->ensureAllLayersRendered(self->contentView);
            return 0;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if (self->contentView)
            {
                auto dpiScale = self->getDpiScale();
                MouseEvent event;
                event.pos = {(float) getXFromLParamEmbedded(lParam) / dpiScale,
                             (float) getYFromLParamEmbedded(lParam) / dpiScale};
                event.type = MouseEventType::Down;
                event.button = (msg == WM_LBUTTONDOWN)   ? MouseButton::Left
                               : (msg == WM_RBUTTONDOWN) ? MouseButton::Right
                                                         : MouseButton::Middle;
                event.modifiers = self->getModifiers();
                self->contentView->dispatchMouseEvent(event);
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if (self->contentView)
            {
                auto dpiScale = self->getDpiScale();
                MouseEvent event;
                event.pos = {(float) getXFromLParamEmbedded(lParam) / dpiScale,
                             (float) getYFromLParamEmbedded(lParam) / dpiScale};
                event.type = MouseEventType::Up;
                event.button = (msg == WM_LBUTTONUP)   ? MouseButton::Left
                               : (msg == WM_RBUTTONUP) ? MouseButton::Right
                                                       : MouseButton::Middle;
                event.modifiers = self->getModifiers();
                self->contentView->dispatchMouseEvent(event);
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (self->contentView)
            {
                auto dpiScale = self->getDpiScale();
                MouseEvent event;
                event.pos = {(float) getXFromLParamEmbedded(lParam) / dpiScale,
                             (float) getYFromLParamEmbedded(lParam) / dpiScale};
                event.type = MouseEventType::Moved;
                event.modifiers = self->getModifiers();
                self->contentView->dispatchMouseEvent(event);
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;

        case WM_KEYDOWN:
        {
            auto vk = static_cast<uint16_t>(wParam);
            self->onKeyDown(vk);
            if (self->contentView)
            {
                KeyEvent event;
                event.keyCode = vk;
                event.type = KeyEventType::Down;
                event.isRepeat = (lParam & 0x40000000) != 0;
                event.modifiers = self->getModifiers();
                self->contentView->keyDown(event);
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;
        }

        case WM_KEYUP:
        {
            auto vk = static_cast<uint16_t>(wParam);
            self->onKeyUp(vk);
            if (self->contentView)
            {
                KeyEvent event;
                event.keyCode = vk;
                event.type = KeyEventType::Up;
                event.modifiers = self->getModifiers();
                self->contentView->keyUp(event);
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;
        }
    }

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
    impl->setContentView(&view);
}

void EmbeddedView::setSize(int width, int height)
{
    impl->setSize(width, height);
}

void* EmbeddedView::getHandle()
{
    return impl->hwnd;
}

} // namespace eacp::Graphics
