#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/Graphics/DComp-Windows.h>

#include <chrono>
#include <iostream>
#include <thread>

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

// The rest of the suite exercises the composition backend without ever
// looking at a pixel — it once passed 386/387 against a backend whose windows
// showed nothing (every visual tree was covered by its own first-attached
// child; see DCOMP-PORT-STATUS.md). These tests capture what DWM actually
// composes for the window and assert on the colors, so "renders at all" and
// "siblings stack in add order" are both pinned by observable output.

namespace
{
// Asks DWM to render the window's full composed content into the DC —
// including the DirectComposition visual tree, which a plain BitBlt or
// default PrintWindow miss. Defined locally: not every SDK header names it.
constexpr auto printWindowFullContent = UINT {0x00000002};

struct FilledView final : View
{
    explicit FilledView(const Color& color)
    {
        layer.setFillColor(color);
        addLayer(layer);
    }

    void resized() override
    {
        layer.setBounds(getLocalBounds());
        auto path = Path();
        path.addRect(getLocalBounds());
        layer.setPath(path);
    }

    ShapeLayer layer;
};

// Two full-size siblings: red attached first, green attached second. The
// later sibling must win the pixels, matching macOS and the WinRT backend.
struct StackedViews final : View
{
    StackedViews() { addChildren({bottom, top}); }

    void resized() override
    {
        bottom.setBounds(getLocalBounds());
        top.setBounds(getLocalBounds());
    }

    FilledView bottom {{1.f, 0.f, 0.f}};
    FilledView top {{0.f, 1.f, 0.f}};
};

struct Rgb
{
    int r = -1;
    int g = -1;
    int b = -1;
};

Rgb clientCenterPixel(HWND hwnd)
{
    RECT client {};
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

void pumpPendingMessages()
{
    auto message = MSG {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool isGreen(const Rgb& pixel)
{
    return pixel.g > 200 && pixel.r >= 0 && pixel.r < 60 && pixel.b >= 0
           && pixel.b < 60;
}
} // namespace

auto tLaterSiblingWinsPixels = test("Composition/laterSiblingWinsPixels") = []
{
    // No desktop compositor (headless CI session, remote host): there is
    // nothing to compose or capture, so there is nothing to assert.
    if (!isCompositorInitialized())
    {
        check(true);
        return;
    }

    auto& environment = Apps::getAppEnvironment();
    auto previousHeadless = environment.headless;
    environment.headless = false;

    {
        auto options = WindowOptions {};
        options.isPrimary = false;

        auto window = Window {options};
        auto content = StackedViews {};
        window.setContentView(content);

        auto hwnd = static_cast<HWND>(window.getHandle());
        auto pixel = Rgb {};

        // DWM picks the committed visual tree up asynchronously, so poll —
        // the loop exits on the first green frame, and the deadline only
        // matters when the backend is genuinely broken.
        for (auto attempt = 0; attempt < 60 && !isGreen(pixel); ++attempt)
        {
            pumpPendingMessages();
            pixel = clientCenterPixel(hwnd);

            if (!isGreen(pixel))
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!isGreen(pixel))
            std::cout << "center pixel after deadline: r=" << pixel.r
                      << " g=" << pixel.g << " b=" << pixel.b << "\n";

        check(isGreen(pixel));
    }

    environment.headless = previousHeadless;
};
