#include "Window.h"
#include "../Layers/NativeLayer-Windows.h"
#include "../Helpers/StringUtils-Windows.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Windows.h>

#include <algorithm>
#include <bitset>
#include <unordered_map>
#include <windowsx.h>

#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.Display.h>
#include <Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

static const wchar_t* WINDOW_CLASS_NAME = L"EACPWindowClass";
static bool windowClassRegistered = false;

namespace
{
std::unordered_map<View*, HWND>& contentViewToHwnd()
{
    static auto map = std::unordered_map<View*, HWND>();
    return map;
}
} // namespace

void registerContentViewHwnd(View* root, HWND hwnd)
{
    contentViewToHwnd()[root] = hwnd;
}

void unregisterContentViewHwnd(View* root)
{
    contentViewToHwnd().erase(root);
}

HWND findHostHwndForView(View* view)
{
    auto* root = view;
    while (root && root->getParent())
        root = root->getParent();

    if (!root)
        return nullptr;

    auto& map = contentViewToHwnd();
    auto it = map.find(root);
    return it == map.end() ? nullptr : it->second;
}

namespace
{
MouseButton mouseButtonFromMessage(UINT msg)
{
    switch (msg)
    {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return MouseButton::Left;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return MouseButton::Right;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return MouseButton::Middle;
        default:
            return MouseButton::Left;
    }
}

MouseEventType mouseEventTypeFromMessage(UINT msg)
{
    switch (msg)
    {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            return MouseEventType::Down;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            return MouseEventType::Up;
        default:
            return MouseEventType::Moved;
    }
}

MouseEvent translateMouseEvent(UINT msg, LPARAM lParam, float dpiScale)
{
    auto event = MouseEvent();
    event.pos = {GET_X_LPARAM(lParam) / dpiScale, GET_Y_LPARAM(lParam) / dpiScale};
    event.type = mouseEventTypeFromMessage(msg);
    event.button = mouseButtonFromMessage(msg);
    return event;
}

KeyEvent translateKeyEvent(KeyEventType type,
                           uint16_t vk,
                           LPARAM lParam,
                           ModifierKeys modifiers)
{
    auto event = KeyEvent();
    event.keyCode = vk;
    event.type = type;
    event.isRepeat = (lParam & 0x40000000) != 0;
    event.modifiers = modifiers;
    return event;
}
} // namespace

struct Window::Native
{
    Native(Window* owner, const WindowOptions& options)
        : ownerWindow(owner)
        , quitCallback(options.onQuit)
    {
        registerWindowClass();
        createWindow(options);
        initializeComposition();
    }

    ~Native()
    {
        if (rootVisual)
            rootVisual.Children().RemoveAll();

        rootVisual = nullptr;
        target = nullptr;

        if (contentView)
            unregisterContentViewHwnd(contentView);

        if (hwnd)
            DestroyWindow(hwnd);
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

    bool isShiftPressed() const { return isKeyPressed(VK_SHIFT); }
    bool isControlPressed() const { return isKeyPressed(VK_CONTROL); }
    bool isAltPressed() const { return isKeyPressed(VK_MENU); }
    bool isCommandPressed() const
    {
        return isKeyPressed(VK_LWIN) || isKeyPressed(VK_RWIN);
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

    Window* ownerWindow;
    HWND hwnd = nullptr;
    View* contentView = nullptr;
    Callback quitCallback = [] {};
    wuc::Desktop::DesktopWindowTarget target {nullptr};
    wuc::ContainerVisual rootVisual {nullptr};

    std::bitset<256> keyState;
};

void Window::Native::setContentView(View* view)
{
    if (contentView && contentView != view)
        unregisterContentViewHwnd(contentView);

    contentView = view;

    if (hwnd && view)
    {
        registerContentViewHwnd(view, hwnd);

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
            ValidateRect(hwnd, nullptr);
            if (self->contentView)
                self->ensureAllLayersRendered(self->contentView);
            return 0;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_MOUSEMOVE:
            if (self->contentView)
            {
                self->contentView->dispatchMouseEvent(
                    translateMouseEvent(msg, lParam, self->getWindowDpiScale()));
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;

        case WM_KEYDOWN:
        {
            auto vk = static_cast<uint16_t>(wParam);
            self->onKeyDown(vk);

            if (self->contentView)
            {
                self->contentView->keyDown(translateKeyEvent(
                    KeyEventType::Down, vk, lParam, self->getModifiers()));
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
                self->contentView->keyUp(translateKeyEvent(
                    KeyEventType::Up, vk, lParam, self->getModifiers()));
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
