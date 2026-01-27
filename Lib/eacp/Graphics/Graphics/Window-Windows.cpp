// Windows implementation of Window using DirectComposition
#include "Window.h"
#include "ShapeLayer.h"
#include "TextLayer.h"
#include "NativeLayer-Windows.h"

#include <algorithm>
#include <cassert>

#define NOMINMAX
#include <Windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <shellscalingapi.h>

namespace eacp::Graphics
{

// getDCompDevice() is defined in D2DFactory-Windows.cpp
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
        initializeDirectComposition();
    }

    ~Native()
    {
        rootVisual.Reset();
        dcompTarget.Reset();
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
        int titleLen = MultiByteToWideChar(CP_UTF8, 0, options.title.c_str(), -1,
                                           nullptr, 0);
        std::wstring wideTitle(titleLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, options.title.c_str(), -1, wideTitle.data(),
                            titleLen);

        // Calculate window size including borders
        RECT rect = {0, 0, options.width, options.height};
        AdjustWindowRect(&rect, style, FALSE);

        hwnd = CreateWindowExW(0, WINDOW_CLASS_NAME, wideTitle.c_str(), style,
                               CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                               rect.bottom - rect.top, nullptr, nullptr,
                               GetModuleHandle(nullptr), this);
        // Don't show window here - wait until DirectComposition is initialized
    }

    void initializeDirectComposition()
    {
        if (!hwnd)
            return;

        auto* dcompDevice = getDCompDevice();
        if (!dcompDevice)
            return;

        // Create composition target for this window
        HRESULT hr =
            dcompDevice->CreateTargetForHwnd(hwnd, TRUE, dcompTarget.GetAddressOf());
        if (FAILED(hr))
            return;

        // Create root visual
        hr = dcompDevice->CreateVisual(rootVisual.GetAddressOf());
        if (FAILED(hr))
            return;

        // Set root visual as the target's root
        hr = dcompTarget->SetRoot(rootVisual.Get());
        if (FAILED(hr))
            return;

        // Commit the initial state
        dcompDevice->Commit();
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
        int titleLen =
            MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
        std::wstring wideTitle(titleLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wideTitle.data(), titleLen);
        SetWindowTextW(hwnd, wideTitle.c_str());
    }

    void setContentView(View* view);
    void commit();
    void ensureAllLayersRendered(View* view);

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam);

    Window* ownerWindow;
    HWND hwnd = nullptr;
    View* contentView;
    Callback quitCallback;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual2> rootVisual;
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
        auto* viewVisual = static_cast<IDCompositionVisual2*>(view->getHandle());
        if (rootVisual && viewVisual)
        {
            rootVisual->AddVisual(viewVisual, TRUE, nullptr);
        }

        // Now that we have a content view, show the window
        showWindow();

        // Ensure all layers are rendered and commit
        ensureAllLayersRendered(view);
        commit();
    }
}

void Window::Native::commit()
{
    auto* dcompDevice = getDCompDevice();
    if (dcompDevice)
    {
        dcompDevice->Commit();
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

                // Re-render all layers and commit
                self->ensureAllLayersRendered(self->contentView);
                self->commit();
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
                self->commit();
            }
            return 0;
        }

        case WM_ERASEBKGND:
            // Prevent background erase flicker
            return 1;

        case WM_PAINT:
        {
            // Paint background with GDI (DirectComposition renders on top)
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // Dark background color (0.12, 0.12, 0.12 in RGB)
            HBRUSH brush = CreateSolidBrush(RGB(31, 31, 31));
            FillRect(hdc, &rc, brush);
            DeleteObject(brush);
            EndPaint(hwnd, &ps);

            // Ensure all layers are rendered and commit changes
            if (self->contentView)
            {
                self->ensureAllLayersRendered(self->contentView);
                self->commit();
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

                // Ensure any changed layers are rendered and commit
                self->ensureAllLayersRendered(self->contentView);
                self->commit();
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

                // Ensure any changed layers are rendered and commit
                self->ensureAllLayersRendered(self->contentView);
                self->commit();
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

                // Ensure any changed layers are rendered and commit
                self->ensureAllLayersRendered(self->contentView);
                self->commit();
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

                // Ensure any changed layers are rendered and commit
                self->ensureAllLayersRendered(self->contentView);
                self->commit();
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

                // Ensure any changed layers are rendered and commit
                self->ensureAllLayersRendered(self->contentView);
                self->commit();
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
