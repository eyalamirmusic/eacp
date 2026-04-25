#include "Window.h"
#include "../Layers/NativeLayer-Windows.h"
#include "../Helpers/StringUtils-Windows.h"

#include <algorithm>
#include <bitset>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.Display.h>
#include <Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>
#include <DispatcherQueue.h>

// Virtual key codes - defined manually to reduce Windows.h dependency
namespace VK
{
constexpr int Shift = 0x10;
constexpr int Control = 0x11;
constexpr int Menu = 0x12; // Alt key
constexpr int LWin = 0x5B;
constexpr int RWin = 0x5C;
} // namespace VK

// Helpers for extracting mouse coordinates from LPARAM
inline int getXFromLParam(LPARAM lp)
{
    return static_cast<int>(static_cast<short>(LOWORD(lp)));
}
inline int getYFromLParam(LPARAM lp)
{
    return static_cast<int>(static_cast<short>(HIWORD(lp)));
}

namespace wuc = winrt::Windows::UI::Composition;
namespace wucore = winrt::Windows::UI::Core;
namespace wui = winrt::Windows::UI::Input;

namespace eacp::Graphics
{

// getWinRTCompositor() is defined in D2DFactory-Windows.cpp
// which is included earlier in the unity build

// Window class name
static const wchar_t* WINDOW_CLASS_NAME = L"EACPWindowClass";
static bool windowClassRegistered = false;

struct Window::Native
{
    Native(Window* owner, const WindowOptions& options)
        : ownerWindow(owner)
        , contentView(nullptr)
        , quitCallback(options.onQuit)
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        keyState.reset();
        registerWindowClass();
        createWindow(options);
        initializeComposition();
        initializePointerInput();
    }

    ~Native()
    {
        if (rootVisual)
            rootVisual.Children().RemoveAll();

        if (inputSource && useWinRTPointerInput)
        {
            inputSource.PointerPressed(pointerPressedToken);
            inputSource.PointerReleased(pointerReleasedToken);
            inputSource.PointerMoved(pointerMovedToken);
        }

        inputSource = nullptr;
        dispatcherController = nullptr;
        rootVisual = nullptr;
        target = nullptr;
        if (hwnd)
        {
            DestroyWindow(hwnd);
        }
    }

    static void registerWindowClass()
    {
        if (windowClassRegistered)
            return;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = WINDOW_CLASS_NAME;

        RegisterClassExW(&wc);
        windowClassRegistered = true;
    }

    void createWindow(const WindowOptions& options)
    {
        DWORD style = WS_OVERLAPPEDWINDOW;

        if (std::ranges::find(options.flags, WindowFlags::Borderless)
            != options.flags.end())
        {
            style = WS_POPUP;
        }

        std::wstring wideTitle = toWideString(options.title);

        auto dpi = GetDpiForSystem();
        auto dpiScale = static_cast<float>(dpi) / 96.f;
        auto physicalWidth = static_cast<int>(options.width * dpiScale);
        auto physicalHeight = static_cast<int>(options.height * dpiScale);

        RECT rect = {0, 0, physicalWidth, physicalHeight};
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);

        hwnd = CreateWindowExW(0,
                               WINDOW_CLASS_NAME,
                               wideTitle.c_str(),
                               style,
                               CW_USEDEFAULT,
                               CW_USEDEFAULT,
                               rect.right - rect.left,
                               rect.bottom - rect.top,
                               nullptr,
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
        auto hr = interop->CreateDesktopWindowTarget(hwnd, 1, abiTarget.put());
        if (FAILED(hr) || !abiTarget)
            return;

        target = abiTarget.as<wuc::Desktop::DesktopWindowTarget>();
        rootVisual = compositor.CreateContainerVisual();

        auto dpiScale = getWindowDpiScale();
        rootVisual.Scale({dpiScale, dpiScale, 1.0f});

        target.Root(rootVisual);
    }

    void initializePointerInput()
    {
        if (!rootVisual)
            return;

        DispatcherQueueOptions dqOptions {};
        dqOptions.dwSize = sizeof(DispatcherQueueOptions);
        dqOptions.threadType = DQTYPE_THREAD_CURRENT;
        dqOptions.apartmentType = DQTAT_COM_NONE;

        ABI::Windows::System::IDispatcherQueueController* controller = nullptr;
        auto hr = CreateDispatcherQueueController(dqOptions, &controller);

        if (FAILED(hr) || !controller)
            return;

        dispatcherController = winrt::Windows::System::DispatcherQueueController {
            controller, winrt::take_ownership_from_abi};

        useWinRTPointerInput = false;
    }

    void showWindow() const
    {
        if (hwnd)
        {
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
    }

    float getWindowDpiScale() const
    {
        try
        {
            auto displayInfo = winrt::Windows::Graphics::Display::
                DisplayInformation::GetForCurrentView();
            return displayInfo.LogicalDpi() / 96.f;
        }
        catch (...)
        {
            auto dpi = GetDpiForWindow(hwnd);
            return dpi / 96.f;
        }
    }

    void setTitle(const std::string& title)
    {
        auto wideTitle = toWideString(title);
        SetWindowTextW(hwnd, wideTitle.c_str());
    }

    // Keyboard state tracking
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

    bool isKeyPressed(uint16_t vk) const { return vk < 256 && keyState.test(vk); }

    bool isShiftPressed() const { return isKeyPressed(VK::Shift); }
    bool isControlPressed() const { return isKeyPressed(VK::Control); }
    bool isAltPressed() const { return isKeyPressed(VK::Menu); }
    bool isCommandPressed() const
    {
        return isKeyPressed(VK::LWin) || isKeyPressed(VK::RWin);
    }

    ModifierKeys getModifiers() const
    {
        return {isShiftPressed(),
                isControlPressed(),
                isAltPressed(),
                isCommandPressed()};
    }

    void setContentView(View* view);
    void ensureAllLayersRendered(const View* view);

    static LRESULT CALLBACK windowProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wParam,
                                       LPARAM lParam);

    // Pointer event handlers for CoreIndependentInputSource
    void onPointerPressed(wucore::CoreIndependentInputSource const& sender,
                          wucore::PointerEventArgs const& args);
    void onPointerReleased(wucore::CoreIndependentInputSource const& sender,
                           wucore::PointerEventArgs const& args);
    void onPointerMoved(wucore::CoreIndependentInputSource const& sender,
                        wucore::PointerEventArgs const& args);

    Window* ownerWindow;
    HWND hwnd = nullptr;
    View* contentView = nullptr;
    Callback quitCallback = [] {};
    wuc::Desktop::DesktopWindowTarget target {nullptr};
    wuc::ContainerVisual rootVisual {nullptr};
    wucore::CoreIndependentInputSource inputSource {nullptr};

    // Dispatcher queue for WinRT input events
    winrt::Windows::System::DispatcherQueueController dispatcherController {nullptr};

    winrt::event_token pointerPressedToken;
    winrt::event_token pointerReleasedToken;
    winrt::event_token pointerMovedToken;
    bool useWinRTPointerInput = false;

    std::bitset<256> keyState;
};

void Window::Native::setContentView(View* view)
{
    contentView = view;

    if (hwnd && view)
    {
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        auto dpiScale = getWindowDpiScale();
        view->setBounds({0.f,
                         0.f,
                         (float) clientRect.right / dpiScale,
                         (float) clientRect.bottom / dpiScale});

        auto* viewVisual = static_cast<wuc::ContainerVisual*>(view->getHandle());

        if (rootVisual && viewVisual)
            rootVisual.Children().InsertAtTop(*viewVisual);

        showWindow();
        ensureAllLayersRendered(view);
    }
}

void Window::Native::ensureAllLayersRendered(const View* view)
{
    if (!view)
        return;

    auto& layers = view->getLayers();
    for (auto* layer: layers)
    {
        auto* native = static_cast<NativeLayerBase*>(layer->getNativeLayer());
        if (native)
        {
            native->ensureContent();
        }
    }

    auto& subviews = view->getSubviews();
    for (auto* subview: subviews)
    {
        ensureAllLayersRendered(subview);
    }
}

void Window::Native::onPointerPressed(wucore::CoreIndependentInputSource const&,
                                      wucore::PointerEventArgs const& args)
{
    if (!contentView)
        return;

    auto point = args.CurrentPoint();
    auto position = point.Position();
    float dpiScale = getWindowDpiScale();

    MouseEvent event;
    event.pos = {position.X / dpiScale, position.Y / dpiScale};
    event.type = MouseEventType::Down;

    // Determine button from pointer properties
    auto props = point.Properties();
    if (props.IsLeftButtonPressed())
        event.button = MouseButton::Left;
    else if (props.IsRightButtonPressed())
        event.button = MouseButton::Right;
    else if (props.IsMiddleButtonPressed())
        event.button = MouseButton::Middle;

    contentView->dispatchMouseEvent(event);
    ensureAllLayersRendered(contentView);
}

void Window::Native::onPointerReleased(wucore::CoreIndependentInputSource const&,
                                       wucore::PointerEventArgs const& args)
{
    if (!contentView)
        return;

    auto point = args.CurrentPoint();
    auto position = point.Position();
    float dpiScale = getWindowDpiScale();

    MouseEvent event;
    event.pos = {position.X / dpiScale, position.Y / dpiScale};
    event.type = MouseEventType::Up;

    // Determine which button was released
    auto props = point.Properties();
    auto update = props.PointerUpdateKind();
    if (update == wui::PointerUpdateKind::LeftButtonReleased)
        event.button = MouseButton::Left;
    else if (update == wui::PointerUpdateKind::RightButtonReleased)
        event.button = MouseButton::Right;
    else if (update == wui::PointerUpdateKind::MiddleButtonReleased)
        event.button = MouseButton::Middle;

    contentView->dispatchMouseEvent(event);
    ensureAllLayersRendered(contentView);
}

void Window::Native::onPointerMoved(wucore::CoreIndependentInputSource const&,
                                    wucore::PointerEventArgs const& args)
{
    if (!contentView)
        return;

    auto point = args.CurrentPoint();
    auto position = point.Position();
    float dpiScale = getWindowDpiScale();

    MouseEvent event;
    event.pos = {position.X / dpiScale, position.Y / dpiScale};
    event.type = MouseEventType::Moved;

    contentView->dispatchMouseEvent(event);
    ensureAllLayersRendered(contentView);
}

LRESULT CALLBACK Window::Native::windowProc(HWND hwnd,
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
    {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg)
    {
        case WM_CLOSE:

            self->quitCallback();

            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SIZE:
            if (self->contentView)
            {
                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                float dpiScale = self->getWindowDpiScale();
                self->contentView->setBounds(
                    Rect(0,
                         0,
                         static_cast<float>(clientRect.right) / dpiScale,
                         static_cast<float>(clientRect.bottom) / dpiScale));
                self->ensureAllLayersRendered(self->contentView);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_DPICHANGED:
        {
            auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd,
                         nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);

            auto dpiScale = self->getWindowDpiScale();
            if (self->rootVisual)
                self->rootVisual.Scale({dpiScale, dpiScale, 1.0f});

            if (self->contentView)
                self->ensureAllLayersRendered(self->contentView);

            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            ValidateRect(hwnd, nullptr);

            if (self->contentView != nullptr)
                self->ensureAllLayersRendered(self->contentView);

            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if (self->useWinRTPointerInput)
                break;
            if (self->contentView)
            {
                float dpiScale = self->getWindowDpiScale();
                MouseEvent event;
                event.pos = {static_cast<float>(getXFromLParam(lParam)) / dpiScale,
                             static_cast<float>(getYFromLParam(lParam)) / dpiScale};
                event.type = MouseEventType::Down;
                event.button = (msg == WM_LBUTTONDOWN)   ? MouseButton::Left
                               : (msg == WM_RBUTTONDOWN) ? MouseButton::Right
                                                         : MouseButton::Middle;
                self->contentView->dispatchMouseEvent(event);
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            // Skip if using WinRT pointer input
            if (self->useWinRTPointerInput)
                break;
            if (self->contentView)
            {
                float dpiScale = self->getWindowDpiScale();
                MouseEvent event;
                event.pos = {static_cast<float>(getXFromLParam(lParam)) / dpiScale,
                             static_cast<float>(getYFromLParam(lParam)) / dpiScale};
                event.type = MouseEventType::Up;
                event.button = (msg == WM_LBUTTONUP)   ? MouseButton::Left
                               : (msg == WM_RBUTTONUP) ? MouseButton::Right
                                                       : MouseButton::Middle;
                self->contentView->dispatchMouseEvent(event);
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (self->useWinRTPointerInput)
                break;

            if (self->contentView)
            {
                float dpiScale = self->getWindowDpiScale();
                MouseEvent event;
                event.pos = {static_cast<float>(getXFromLParam(lParam)) / dpiScale,
                             static_cast<float>(getYFromLParam(lParam)) / dpiScale};
                event.type = MouseEventType::Moved;
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

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(this, optionsToUse)
{
}

Window::~Window() = default;

void Window::setTitle(const std::string& title)
{
    impl->setTitle(title);
}

void* Window::getHandle()
{
    return impl->hwnd;
}

void* Window::getContentViewHandle()
{
    return impl->hwnd;
}

void Window::setContentView(View& view)
{
    impl->setContentView(&view);
}

bool Window::isKeyPressed(uint16_t virtualKeyCode) const
{
    return impl->isKeyPressed(virtualKeyCode);
}

bool Window::isShiftPressed() const
{
    return impl->isShiftPressed();
}

bool Window::isControlPressed() const
{
    return impl->isControlPressed();
}

bool Window::isAltPressed() const
{
    return impl->isAltPressed();
}

bool Window::isCommandPressed() const
{
    return impl->isCommandPressed();
}

ModifierKeys Window::getModifiers() const
{
    return impl->getModifiers();
}

} // namespace eacp::Graphics
