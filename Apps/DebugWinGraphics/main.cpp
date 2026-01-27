#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <cmath>
#include <vector>

using Microsoft::WRL::ComPtr;

constexpr auto WINDOW_WIDTH = 800;
constexpr auto WINDOW_HEIGHT = 600;
constexpr auto NUM_SHAPES = 5;
constexpr auto ANIMATION_TIMER_ID = 1;
constexpr auto ANIMATION_INTERVAL_MS = 16;

struct AnimatedShape {
    float x, y;
    float velocityX, velocityY;
    float size;
    D2D1_COLOR_F color;
    int shapeType;

    float surfaceSize() const { return size * 2.5f; }
    float center() const { return size * 1.25f; }
    float radius() const { return size * 0.9f; }
};

D2D1_COLOR_F shapeColor(int index) {
    constexpr D2D1_COLOR_F colors[] = {
        {1.0f, 0.3f, 0.3f, 0.9f},
        {0.3f, 1.0f, 0.3f, 0.9f},
        {0.3f, 0.3f, 1.0f, 0.9f},
        {1.0f, 1.0f, 0.3f, 0.9f},
        {1.0f, 0.3f, 1.0f, 0.9f},
    };
    return colors[index % 5];
}

class Application {
public:
    HWND hwnd = nullptr;

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDCompositionDesktopDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual2> rootVisual;
    std::vector<ComPtr<IDCompositionVisual2>> shapeVisuals;
    std::vector<ComPtr<IDCompositionVirtualSurface>> shapeSurfaces;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    std::vector<AnimatedShape> shapes;

    bool Initialize(HWND window);
    void CreateShapes();
    void UpdateAnimation();
    void Cleanup();

private:
    void DrawCircle(ID2D1DeviceContext* dc, const AnimatedShape& shape, POINT offset);
    void DrawRoundedRect(ID2D1DeviceContext* dc, const AnimatedShape& shape, POINT offset);
    void DrawStar(ID2D1DeviceContext* dc, const AnimatedShape& shape, POINT offset);
    void DrawShapeToSurface(IDCompositionVirtualSurface* surface, const AnimatedShape& shape);
    ComPtr<ID2D1SolidColorBrush> CreateBrush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& color);
};

Application g_app;

ComPtr<ID2D1SolidColorBrush> Application::CreateBrush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(color, &brush);
    return brush;
}

void Application::DrawCircle(ID2D1DeviceContext* dc, const AnimatedShape& shape, POINT offset) {
    auto cx = shape.center() + offset.x;
    auto cy = shape.center() + offset.y;
    auto ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), shape.radius(), shape.radius());

    dc->FillEllipse(ellipse, CreateBrush(dc, shape.color).Get());
    dc->DrawEllipse(ellipse, CreateBrush(dc, {0.2f, 0.2f, 0.2f, 1.0f}).Get(), 3.0f);
}

void Application::DrawRoundedRect(ID2D1DeviceContext* dc, const AnimatedShape& shape, POINT offset) {
    auto cx = shape.center() + offset.x;
    auto cy = shape.center() + offset.y;
    auto r = shape.radius();
    auto cornerRadius = r * 0.2f;
    auto rect = D2D1::RoundedRect(D2D1::RectF(cx - r, cy - r, cx + r, cy + r), cornerRadius, cornerRadius);

    dc->FillRoundedRectangle(rect, CreateBrush(dc, shape.color).Get());
    dc->DrawRoundedRectangle(rect, CreateBrush(dc, {0.2f, 0.2f, 0.2f, 1.0f}).Get(), 3.0f);
}

void Application::DrawStar(ID2D1DeviceContext* dc, const AnimatedShape& shape, POINT offset) {
    auto cx = shape.center() + offset.x;
    auto cy = shape.center() + offset.y;
    auto outerR = shape.radius();
    auto innerR = outerR * 0.4f;
    constexpr auto numPoints = 10;
    constexpr auto pi = 3.14159f;

    ComPtr<ID2D1PathGeometry> pathGeometry;
    d2dFactory->CreatePathGeometry(&pathGeometry);

    ComPtr<ID2D1GeometrySink> sink;
    pathGeometry->Open(&sink);

    for (auto i = 0; i < numPoints; i++) {
        auto angle = static_cast<float>(i) * pi * 2.0f / numPoints - pi / 2.0f;
        auto radius = (i % 2 == 0) ? outerR : innerR;
        auto px = cx + cosf(angle) * radius;
        auto py = cy + sinf(angle) * radius;

        if (i == 0) {
            sink->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_FILLED);
        } else {
            sink->AddLine(D2D1::Point2F(px, py));
        }
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    dc->FillGeometry(pathGeometry.Get(), CreateBrush(dc, shape.color).Get());
    dc->DrawGeometry(pathGeometry.Get(), CreateBrush(dc, {0.2f, 0.2f, 0.2f, 1.0f}).Get(), 3.0f);
}

void Application::DrawShapeToSurface(IDCompositionVirtualSurface* surface, const AnimatedShape& shape) {
    POINT offset;
    ComPtr<ID2D1DeviceContext> dc;
    auto size = static_cast<int>(shape.surfaceSize());
    RECT updateRect = {0, 0, size, size};

    if (FAILED(surface->BeginDraw(&updateRect, IID_PPV_ARGS(&dc), &offset))) return;

    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    switch (shape.shapeType) {
        case 0: DrawCircle(dc.Get(), shape, offset); break;
        case 1: DrawRoundedRect(dc.Get(), shape, offset); break;
        case 2: DrawStar(dc.Get(), shape, offset); break;
    }

    surface->EndDraw();
}

bool Application::Initialize(HWND window) {
    hwnd = window;

    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL featureLevel;

    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED,
            featureLevels, 1, D3D11_SDK_VERSION, &d3dDevice, &featureLevel, nullptr)))
        return false;

    if (FAILED(d3dDevice.As(&dxgiDevice))) return false;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf()))) return false;
    if (FAILED(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice))) return false;
    if (FAILED(DCompositionCreateDevice2(d2dDevice.Get(), IID_PPV_ARGS(&dcompDevice)))) return false;
    if (FAILED(dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget))) return false;
    if (FAILED(dcompDevice->CreateVisual(&rootVisual))) return false;
    if (FAILED(dcompTarget->SetRoot(rootVisual.Get()))) return false;

    CreateShapes();

    return SUCCEEDED(dcompDevice->Commit());
}

void Application::CreateShapes() {
    shapes.resize(NUM_SHAPES);
    shapeVisuals.resize(NUM_SHAPES);
    shapeSurfaces.resize(NUM_SHAPES);

    for (auto i = 0; i < NUM_SHAPES; i++) {
        auto& shape = shapes[i];
        shape.size = 60.0f + (i * 15.0f);
        shape.x = 100.0f + (i * 120.0f);
        shape.y = 100.0f + (i * 80.0f);
        shape.velocityX = 2.0f + (i * 0.5f) * ((i % 2) ? 1.0f : -1.0f);
        shape.velocityY = 1.5f + (i * 0.3f) * ((i % 3) ? 1.0f : -1.0f);
        shape.color = shapeColor(i);
        shape.shapeType = i % 3;

        if (FAILED(dcompDevice->CreateVisual(&shapeVisuals[i]))) continue;

        auto surfaceSize = static_cast<int>(shape.surfaceSize());
        if (FAILED(dcompDevice->CreateVirtualSurface(surfaceSize, surfaceSize,
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &shapeSurfaces[i])))
            continue;

        DrawShapeToSurface(shapeSurfaces[i].Get(), shape);
        shapeVisuals[i]->SetContent(shapeSurfaces[i].Get());
        shapeVisuals[i]->SetOffsetX(shape.x - shape.center());
        shapeVisuals[i]->SetOffsetY(shape.y - shape.center());

        auto insertAfter = (i == 0) ? nullptr : shapeVisuals[i - 1].Get();
        rootVisual->AddVisual(shapeVisuals[i].Get(), TRUE, insertAfter);
    }
}

void Application::UpdateAnimation() {
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    auto width = static_cast<float>(clientRect.right);
    auto height = static_cast<float>(clientRect.bottom);

    for (auto i = 0; i < NUM_SHAPES; i++) {
        auto& shape = shapes[i];

        shape.x += shape.velocityX;
        shape.y += shape.velocityY;

        if (shape.x - shape.size < 0 || shape.x + shape.size > width) {
            shape.x = (shape.x - shape.size < 0) ? shape.size : width - shape.size;
            shape.velocityX = -shape.velocityX;
        }

        if (shape.y - shape.size < 0 || shape.y + shape.size > height) {
            shape.y = (shape.y - shape.size < 0) ? shape.size : height - shape.size;
            shape.velocityY = -shape.velocityY;
        }

        if (shapeVisuals[i]) {
            shapeVisuals[i]->SetOffsetX(shape.x - shape.center());
            shapeVisuals[i]->SetOffsetY(shape.y - shape.center());
        }
    }

    dcompDevice->Commit();
}

void Application::Cleanup() {
    shapeSurfaces.clear();
    shapeVisuals.clear();
    rootVisual.Reset();
    dcompTarget.Reset();
    dcompDevice.Reset();
    d2dDevice.Reset();
    d2dFactory.Reset();
    dxgiDevice.Reset();
    d3dDevice.Reset();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            if (!g_app.Initialize(hwnd)) {
                MessageBoxW(hwnd, L"Failed to initialize DirectComposition", L"Error", MB_OK | MB_ICONERROR);
                return -1;
            }
            SetTimer(hwnd, ANIMATION_TIMER_ID, ANIMATION_INTERVAL_MS, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == ANIMATION_TIMER_ID) g_app.UpdateAnimation();
            return 0;

        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            auto hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            auto brush = CreateSolidBrush(RGB(30, 30, 40));
            FillRect(hdc, &rc, brush);
            DeleteObject(brush);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) PostQuitMessage(0);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, ANIMATION_TIMER_ID);
            g_app.Cleanup();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DirectCompositionDemo";

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    RECT rect = {0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    auto hwnd = CreateWindowExW(0, L"DirectCompositionDemo", L"DirectComposition Animated Layers Demo",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
