// Windows implementation of Window using Windows.UI.Composition
#include "Window.h"
#include "ShapeLayer.h"
#include "TextLayer.h"
#include "NativeLayer-Windows.h"
#include "../Helpers/StringUtils-Windows.h"

#include <algorithm>
#include <cassert>

#define NOMINMAX
#include <Windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <shellscalingapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

// getWinRTCompositor() is defined in D2DFactory-Windows.cpp
// which is included earlier in the unity build

using Microsoft::WRL::ComPtr;

// Window class name
static auto WINDOW_CLASS_NAME = L"EACPWindowClass";
static auto windowClassRegistered = false;

struct Window::Native
{
    Native(Window* owner, const WindowOptions& options)
        : ownerWindow(owner)
        , contentView(nullptr)
        , quitCallback(options.onQuit)
    {
        registerWindowClass();
        createWindow(options);
        initializeComposition();
    }

    ~Native()
    {
        rootVisual = nullptr;
        target = nullptr;
        if (hwnd)
        {
            DestroyWindow(hwnd);
        }
    }

    void registerWindowClass()
    {
        if (windowClassRegistered)
            return;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr; // We'll paint background in WM_PAINT
        wc.lpszClassName = WINDOW_CLASS_NAME;

        RegisterClassExW(&wc);
        windowClassRegistered = true;
    }

    void createWindow(const WindowOptions& options)
    {
        DWORD style = WS_OVERLAPPEDWINDOW;

        // Map window flags (simplified)
        if (std::find(options.flags.begin(), options.flags.end(),
                      WindowFlags::Borderless) != options.flags.end())
        {
            style = WS_POPUP;
        }

        // Convert title to wide string
        std::wstring wideTitle = toWideString(options.title);

        // Calculate window size including borders
        RECT rect = {0, 0, options.width, options.height};
        AdjustWindowRect(&rect, style, FALSE);

        hwnd = CreateWindowExW(0, WINDOW_CLASS_NAME, wideTitle.c_str(), style,
                               CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                               rect.bottom - rect.top, nullptr, nullptr,
                               GetModuleHandle(nullptr), this);
        // Don't show window here - wait until composition is initialized
    }

    void initializeComposition()
    {
        if (!hwnd)
            return;

        auto compositor = getWinRTCompositor();
        if (!compositor)
            return;

        // Create DesktopWindowTarget via interop
        auto interop =
            compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        winrt::com_ptr<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget> abiTarget;
        HRESULT hr = interop->CreateDesktopWindowTarget(hwnd, TRUE, abiTarget.put());
        if (FAILED(hr) || !abiTarget)
            return;

        target = abiTarget.as<wuc::Desktop::DesktopWindowTarget>();

        // Create root container visual
        rootVisual = compositor.CreateContainerVisual();

        // Set root visual as the target's root
        target.Root(rootVisual);
    }

    void showWindow()
    {
        if (hwnd)
        {
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
    }

    float getWindowDpiScale()
    {
        UINT dpi = GetDpiForWindow(hwnd);
        return dpi / 96.0f;
    }

    void setTitle(const std::string& title)
    {
        std::wstring wideTitle = toWideString(title);
        SetWindowTextW(hwnd, wideTitle.c_str());
    }

    void setContentView(View* view);
    void ensureAllLayersRendered(View* view);

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam);

    Window* ownerWindow;
    HWND hwnd = nullptr;
    View* contentView;
    Callback quitCallback;
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual rootVisual{nullptr};
};

void Window::Native::setContentView(View* view)
{
    contentView = view;
    // Trigger initial resize/layout
    if (hwnd && view)
    {
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        // Convert from physical pixels to DIPs for the view system
        float dpiScale = getWindowDpiScale();
        view->setBounds(Rect(0, 0, static_cast<float>(clientRect.right) / dpiScale,
                             static_cast<float>(clientRect.bottom) / dpiScale));

        // Attach content view's visual to root visual
        auto* viewVisual = static_cast<wuc::ContainerVisual*>(view->getHandle());
        if (rootVisual && viewVisual)
        {
            rootVisual.Children().InsertAtTop(*viewVisual);
        }

        // Now that we have a content view, show the window
        showWindow();

        // Ensure all layers are rendered
        ensureAllLayersRendered(view);
    }
}

void Window::Native::ensureAllLayersRendered(View* view)
{
    if (!view)
        return;

    // Ensure all layers in this view have their content rendered
    auto& layers = view->getLayers();
    for (auto* layer : layers)
    {
        auto* native = static_cast<NativeLayerBase*>(layer->getNativeLayer());
        if (native)
        {
            native->ensureContent();
        }
    }

    // Recursively process child views
    auto& subviews = view->getSubviews();
    for (auto* subview : subviews)
    {
        ensureAllLayersRendered(subview);
    }
}

LRESULT CALLBACK Window::Native::windowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                            LPARAM lParam)
{
    Native* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<Native*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd = hwnd;
    }
    else
    {
        self = reinterpret_cast<Native*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (!self)
    {
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    switch (msg)
    {
        case WM_CLOSE:
            if (self->quitCallback)
            {
                self->quitCallback();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SIZE:
            if (self->contentView)
            {
                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                // Convert from physical pixels to DIPs for the view system
                float dpiScale = self->getWindowDpiScale();
                self->contentView->setBounds(
                    Rect(0, 0, static_cast<float>(clientRect.right) / dpiScale,
                         static_cast<float>(clientRect.bottom) / dpiScale));

                // Re-render all layers
                self->ensureAllLayersRendered(self->contentView);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_DPICHANGED:
        {
            // Resize window to suggested size
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);

            // Re-render all layers at new DPI
            if (self->contentView)
            {
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;
        }

        case WM_ERASEBKGND:
            // Prevent background erase flicker
            return 1;

        case WM_PAINT:
        {
            // Paint background with GDI (composition renders on top)
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // Dark background color (0.12, 0.12, 0.12 in RGB)
            HBRUSH brush = CreateSolidBrush(RGB(31, 31, 31));
            FillRect(hdc, &rc, brush);
            DeleteObject(brush);
            EndPaint(hwnd, &ps);

            // Ensure all layers are rendered
            if (self->contentView)
            {
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if (self->contentView)
            {
                float dpiScale = self->getWindowDpiScale();
                MouseEvent event;
                event.pos = {static_cast<float>(GET_X_LPARAM(lParam)) / dpiScale,
                             static_cast<float>(GET_Y_LPARAM(lParam)) / dpiScale};
                event.type = MouseEventType::Down;
                event.button = (msg == WM_LBUTTONDOWN)   ? MouseButton::Left
                               : (msg == WM_RBUTTONDOWN) ? MouseButton::Right
                                                         : MouseButton::Middle;
                self->contentView->dispatchMouseEvent(event);

                // Ensure any changed layers are rendered
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if (self->contentView)
            {
                float dpiScale = self->getWindowDpiScale();
                MouseEvent event;
                event.pos = {static_cast<float>(GET_X_LPARAM(lParam)) / dpiScale,
                             static_cast<float>(GET_Y_LPARAM(lParam)) / dpiScale};
                event.type = MouseEventType::Up;
                event.button = (msg == WM_LBUTTONUP)   ? MouseButton::Left
                               : (msg == WM_RBUTTONUP) ? MouseButton::Right
                                                       : MouseButton::Middle;
                self->contentView->dispatchMouseEvent(event);

                // Ensure any changed layers are rendered
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (self->contentView)
            {
                float dpiScale = self->getWindowDpiScale();
                MouseEvent event;
                event.pos = {static_cast<float>(GET_X_LPARAM(lParam)) / dpiScale,
                             static_cast<float>(GET_Y_LPARAM(lParam)) / dpiScale};
                event.type = MouseEventType::Moved;
                self->contentView->dispatchMouseEvent(event);

                // Ensure any changed layers are rendered
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;

        case WM_KEYDOWN:
            if (self->contentView)
            {
                KeyEvent event;
                event.keyCode = static_cast<uint16_t>(wParam);
                event.type = KeyEventType::Down;
                event.isRepeat = (lParam & 0x40000000) != 0;
                event.modifiers = Keyboard::getModifiers();
                self->contentView->keyDown(event);

                // Ensure any changed layers are rendered
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;

        case WM_KEYUP:
            if (self->contentView)
            {
                KeyEvent event;
                event.keyCode = static_cast<uint16_t>(wParam);
                event.type = KeyEventType::Up;
                event.modifiers = Keyboard::getModifiers();
                self->contentView->keyUp(event);

                // Ensure any changed layers are rendered
                self->ensureAllLayersRendered(self->contentView);
            }
            return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
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

void Window::setContentView(View& view)
{
    impl->setContentView(&view);
}

} // namespace eacp::Graphics
