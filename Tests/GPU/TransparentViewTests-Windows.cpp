#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/Graphics/DComp-Windows.h>
#include <eacp/Graphics/Graphics.h>

#include <chrono>
#include <iostream>
#include <thread>

// GPUView::setTransparent, read back as the pixels DWM composes. A top-level
// Window's composition target is topmost, so its visual tree is drawn over the
// window's child HWNDs: a GPUView over a NativeChildSurface covers the foreign
// content under it, and what shows there is decided by the swapchain's alpha
// mode alone. A red GDI child stands in for a hosted plugin's editor.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
constexpr auto windowSize = 160;

struct Rgb
{
    int r = -1;
    int g = -1;
    int b = -1;
};

bool isRed(const Rgb& pixel)
{
    return pixel.r > 200 && pixel.g >= 0 && pixel.g < 60 && pixel.b >= 0
           && pixel.b < 60;
}

bool isBlack(const Rgb& pixel)
{
    return pixel.r >= 0 && pixel.r < 40 && pixel.g >= 0 && pixel.g < 40
           && pixel.b >= 0 && pixel.b < 40;
}

LRESULT CALLBACK redChildProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PAINT)
    {
        auto paint = PAINTSTRUCT {};
        auto dc = BeginPaint(hwnd, &paint);
        auto client = RECT {};
        GetClientRect(hwnd, &client);
        auto brush = CreateSolidBrush(RGB(255, 0, 0));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &paint);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

const wchar_t* redChildClass()
{
    static const auto registered = []
    {
        auto windowClass = WNDCLASSW {};
        windowClass.lpfnWndProc = redChildProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"EACPTransparentViewTestRedChild";
        return RegisterClassW(&windowClass) != 0;
    }();

    return registered ? L"EACPTransparentViewTestRedChild" : nullptr;
}

struct ClearedView final : GPU::GPUView
{
    void render(GPU::Frame& frame) override
    {
        auto pass = frame.beginPass({Color::black(0.f)});
    }
};

struct HostedContent final : View
{
    HostedContent() { addChildren({surface, overlay}); }

    void resized() override
    {
        surface.setBounds(getLocalBounds());
        overlay.setBounds(getLocalBounds());
    }

    NativeChildSurface surface;
    ClearedView overlay;
};

void pumpPendingMessages()
{
    auto message = MSG {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

// Asks DWM for the window's composed content — the child windows and the
// composition tree both, stacked as they are on screen — rather than reading
// the desktop, where whatever else a CI runner has open can cover the window.
// Defined locally: not every SDK header names it.
constexpr auto printWindowFullContent = UINT {0x00000002};

Rgb composedPixelAtClientCenter(HWND hwnd)
{
    auto client = RECT {};
    GetClientRect(hwnd, &client);
    auto width = static_cast<int>(client.right);
    auto height = static_cast<int>(client.bottom);

    if (width <= 0 || height <= 0)
        return {};

    auto screenDc = GetDC(nullptr);
    auto memoryDc = CreateCompatibleDC(screenDc);
    auto bitmap = CreateCompatibleBitmap(screenDc, width, height);
    auto previousBitmap = SelectObject(memoryDc, bitmap);

    auto printed =
        PrintWindow(hwnd, memoryDc, PW_CLIENTONLY | printWindowFullContent);
    auto color = printed ? GetPixel(memoryDc, width / 2, height / 2) : CLR_INVALID;

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);

    if (color == CLR_INVALID)
        return {};

    return {GetRValue(color), GetGValue(color), GetBValue(color)};
}

template <typename Predicate>
Rgb waitForPixel(HWND hwnd, Predicate&& accept)
{
    auto pixel = Rgb {};

    for (auto attempt = 0; attempt < 60; ++attempt)
    {
        pumpPendingMessages();
        pixel = composedPixelAtClientCenter(hwnd);

        if (accept(pixel))
            return pixel;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "center pixel after deadline: r=" << pixel.r << " g=" << pixel.g
              << " b=" << pixel.b << "\n";
    return pixel;
}

// Runs `body` with a shown window whose content is a red child under a GPUView,
// or skips where there is no device, no compositor or no desktop to read.
template <typename Body>
void withHostedWindow(Body&& body)
{
    if (!GPU::Device::shared().isValid() || !isCompositorInitialized()
        || redChildClass() == nullptr)
        return;

    auto& environment = Apps::getAppEnvironment();
    auto previousHeadless = environment.headless;
    environment.headless = false;

    {
        auto options = WindowOptions {};
        options.isPrimary = false;
        options.width = windowSize;
        options.height = windowSize;

        auto content = HostedContent {};
        auto window = Window {content, options};
        auto hwnd = static_cast<HWND>(window.getHandle());
        auto parent = static_cast<HWND>(content.surface.getNativeParentHandle());

        if (hwnd != nullptr && parent != nullptr)
        {
            auto child = CreateWindowExW(0,
                                         redChildClass(),
                                         L"",
                                         WS_CHILD | WS_VISIBLE,
                                         0,
                                         0,
                                         windowSize * 4,
                                         windowSize * 4,
                                         parent,
                                         nullptr,
                                         GetModuleHandleW(nullptr),
                                         nullptr);

            body(content, hwnd);

            if (child != nullptr)
                DestroyWindow(child);
        }
    }

    environment.headless = previousHeadless;
}
} // namespace

auto tTransparentShowsTheChild =
    test("TransparentGPUView/aClearedTransparentViewShowsTheChildWindow") = []
{
    withHostedWindow(
        [](HostedContent& content, HWND hwnd)
        {
            content.overlay.setTransparent(true);
            content.overlay.repaint();

            check(content.overlay.isTransparent());
            check(isRed(waitForPixel(hwnd, isRed)));
        });
};

auto tOpaqueCoversTheChild =
    test("TransparentGPUView/anOpaqueViewCoversTheChildWindowInBlack") = []
{
    withHostedWindow(
        [](HostedContent& content, HWND hwnd)
        {
            content.overlay.repaint();

            check(!content.overlay.isTransparent());
            check(isBlack(waitForPixel(hwnd, isBlack)));
        });
};

auto tTurnedOnLaterRebuilds =
    test("TransparentGPUView/turningItOnAfterAFrameRebuildsTheSwapchain") = []
{
    withHostedWindow(
        [](HostedContent& content, HWND hwnd)
        {
            content.overlay.repaint();
            waitForPixel(hwnd, isBlack);

            content.overlay.setTransparent(true);

            check(isRed(waitForPixel(hwnd, isRed)));
        });
};
