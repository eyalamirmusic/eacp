// Windows implementation of Window using Direct2D
#include "Window.h"
#include "ShapeLayer.h"
#include "TextLayer.h"
#include "NativeLayer-Windows.h"

#include <algorithm>
#include <cassert>

#define NOMINMAX
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windowsx.h>

// Forward declaration of factory access
namespace eacp::Graphics
{
ID2D1Factory* getD2DFactory();
IDWriteFactory* getDWriteFactory();
} // namespace eacp::Graphics

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

// Window class name
static auto WINDOW_CLASS_NAME = L"EACPWindowClass";
static auto windowClassRegistered = false;

// Native layer structures matching the actual implementations
struct ShapeLayerNative : NativeLayerBase
{
    ID2D1PathGeometry* pathGeometry = nullptr;
    Color fillColor;
    LinearGradient gradient;
    bool useGradient = false;
    bool hasFill = false;
    Color strokeColor;
    float strokeWidth = 1.0f;
    bool hasStroke = false;
};

struct TextLayerNative : NativeLayerBase
{
    std::wstring text;
    ComPtr<IDWriteTextFormat> textFormat;
    Color color {1.0f, 1.0f, 1.0f, 1.0f};
};

struct Window::Native
{
    Native(Window* owner, const WindowOptions& options)
        : ownerWindow(owner)
        , contentView(nullptr)
        , quitCallback(options.onQuit)
    {
        registerWindowClass();
        createWindow(options);
        createRenderTarget();
    }

    ~Native()
    {
        renderTarget.Reset();
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
        wc.hbrBackground = nullptr; // D2D handles background
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

        if (hwnd)
        {
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
    }

    void createRenderTarget()
    {
        if (!hwnd)
            return;

        auto* factory = getD2DFactory();
        if (!factory)
            return;

        RECT rc;
        GetClientRect(hwnd, &rc);

        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

        factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, size), renderTarget.GetAddressOf());
    }

    void resizeRenderTarget()
    {
        if (renderTarget)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
            renderTarget->Resize(size);
        }
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
    void render();
    void renderView(View* view, const Point& offset);
    void renderShapeLayer(ShapeLayerNative* layer, const Point& offset);
    void renderTextLayer(TextLayerNative* layer, const Point& offset);

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam);

    Window* ownerWindow;
    HWND hwnd = nullptr;
    View* contentView;
    Callback quitCallback;
    ComPtr<ID2D1HwndRenderTarget> renderTarget;
};

void Window::Native::setContentView(View* view)
{
    contentView = view;
    // Trigger initial resize/layout
    if (hwnd && view)
    {
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        view->setBounds(Rect(0, 0, static_cast<float>(clientRect.right),
                             static_cast<float>(clientRect.bottom)));
    }
}

void Window::Native::render()
{
    if (!renderTarget || !contentView)
        return;

    renderTarget->BeginDraw();

    // Clear with dark background
    renderTarget->Clear(D2D1::ColorF(0.12f, 0.12f, 0.12f));

    // Render the view hierarchy
    renderView(contentView, {0, 0});

    renderTarget->EndDraw();
}

void Window::Native::renderView(View* view, const Point& offset)
{
    if (!view)
        return;

    auto bounds = view->getBounds();
    auto viewOffset = Point {offset.x + bounds.x, offset.y + bounds.y};

    // Render all layers attached to this view
    for (auto* layer : view->getLayers())
    {
        if (auto* shapeLayer = dynamic_cast<ShapeLayer*>(layer))
        {
            auto* native = static_cast<ShapeLayerNative*>(shapeLayer->getNativeLayer());
            renderShapeLayer(native, viewOffset);
        }
        else if (auto* textLayer = dynamic_cast<TextLayer*>(layer))
        {
            auto* native = static_cast<TextLayerNative*>(textLayer->getNativeLayer());
            renderTextLayer(native, viewOffset);
        }
    }

    // Recursively render child views
    for (auto* subview : view->getSubviews())
    {
        renderView(subview, viewOffset);
    }
}

void Window::Native::renderShapeLayer(ShapeLayerNative* layer, const Point& offset)
{
    if (!layer || layer->hidden || !layer->pathGeometry)
        return;

    // Create transform for position
    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Translation(
        offset.x + layer->position.x, offset.y + layer->position.y);
    renderTarget->SetTransform(transform);

    // Fill
    if (layer->hasFill)
    {
        if (layer->useGradient && !layer->gradient.stops.empty())
        {
            // Create gradient brush
            D2D1_GRADIENT_STOP stops[8];
            UINT32 stopCount =
                static_cast<UINT32>((std::min)(layer->gradient.stops.size(), size_t(8)));

            for (UINT32 i = 0; i < stopCount; ++i)
            {
                auto& stop = layer->gradient.stops[i];
                stops[i].position = stop.position;
                stops[i].color =
                    D2D1::ColorF(stop.color.r, stop.color.g, stop.color.b, stop.color.a);
            }

            ComPtr<ID2D1GradientStopCollection> stopCollection;
            renderTarget->CreateGradientStopCollection(stops, stopCount,
                                                       stopCollection.GetAddressOf());

            if (stopCollection)
            {
                ComPtr<ID2D1LinearGradientBrush> gradientBrush;
                renderTarget->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(layer->gradient.start.x, layer->gradient.start.y),
                        D2D1::Point2F(layer->gradient.end.x, layer->gradient.end.y)),
                    stopCollection.Get(), gradientBrush.GetAddressOf());

                if (gradientBrush)
                {
                    gradientBrush->SetOpacity(layer->opacity);
                    renderTarget->FillGeometry(layer->pathGeometry, gradientBrush.Get());
                }
            }
        }
        else
        {
            // Solid color fill
            ComPtr<ID2D1SolidColorBrush> brush;
            renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(layer->fillColor.r, layer->fillColor.g, layer->fillColor.b,
                             layer->fillColor.a * layer->opacity),
                brush.GetAddressOf());

            if (brush)
            {
                renderTarget->FillGeometry(layer->pathGeometry, brush.Get());
            }
        }
    }

    // Stroke
    if (layer->hasStroke && layer->strokeWidth > 0)
    {
        ComPtr<ID2D1SolidColorBrush> strokeBrush;
        renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(layer->strokeColor.r, layer->strokeColor.g,
                         layer->strokeColor.b, layer->strokeColor.a * layer->opacity),
            strokeBrush.GetAddressOf());

        if (strokeBrush)
        {
            renderTarget->DrawGeometry(layer->pathGeometry, strokeBrush.Get(),
                                       layer->strokeWidth);
        }
    }

    renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Window::Native::renderTextLayer(TextLayerNative* layer, const Point& offset)
{
    if (!layer || layer->hidden || layer->text.empty() || !layer->textFormat)
        return;

    ComPtr<ID2D1SolidColorBrush> brush;
    renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(layer->color.r, layer->color.g, layer->color.b,
                     layer->color.a * layer->opacity),
        brush.GetAddressOf());

    if (!brush)
        return;

    D2D1_RECT_F layoutRect = D2D1::RectF(
        offset.x + layer->position.x, offset.y + layer->position.y,
        offset.x + layer->position.x + layer->bounds.w,
        offset.y + layer->position.y + layer->bounds.h);

    renderTarget->DrawText(layer->text.c_str(),
                           static_cast<UINT32>(layer->text.length()),
                           layer->textFormat.Get(), layoutRect, brush.Get());
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
            self->resizeRenderTarget();
            if (self->contentView)
            {
                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                self->contentView->setBounds(
                    Rect(0, 0, static_cast<float>(clientRect.right),
                         static_cast<float>(clientRect.bottom)));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            self->render();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if (self->contentView)
            {
                MouseEvent event;
                event.pos = {static_cast<float>(GET_X_LPARAM(lParam)),
                             static_cast<float>(GET_Y_LPARAM(lParam))};
                event.type = MouseEventType::Down;
                event.button = (msg == WM_LBUTTONDOWN)   ? MouseButton::Left
                               : (msg == WM_RBUTTONDOWN) ? MouseButton::Right
                                                         : MouseButton::Middle;
                self->contentView->dispatchMouseEvent(event);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if (self->contentView)
            {
                MouseEvent event;
                event.pos = {static_cast<float>(GET_X_LPARAM(lParam)),
                             static_cast<float>(GET_Y_LPARAM(lParam))};
                event.type = MouseEventType::Up;
                event.button = (msg == WM_LBUTTONUP)   ? MouseButton::Left
                               : (msg == WM_RBUTTONUP) ? MouseButton::Right
                                                       : MouseButton::Middle;
                self->contentView->dispatchMouseEvent(event);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (self->contentView)
            {
                MouseEvent event;
                event.pos = {static_cast<float>(GET_X_LPARAM(lParam)),
                             static_cast<float>(GET_Y_LPARAM(lParam))};
                event.type = MouseEventType::Moved;
                self->contentView->dispatchMouseEvent(event);
                InvalidateRect(hwnd, nullptr, FALSE);
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
