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

// The pixel at a fraction of the client area's width and height.
Rgb clientPixelAt(HWND hwnd, float x, float y)
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
    auto color = printed ? GetPixel(memoryDc,
                                    static_cast<int>(static_cast<float>(width) * x),
                                    static_cast<int>(static_cast<float>(height) * y))
                         : CLR_INVALID;

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);

    if (color == CLR_INVALID)
        return {};

    return {GetRValue(color), GetGValue(color), GetBValue(color)};
}

Rgb clientCenterPixel(HWND hwnd)
{
    return clientPixelAt(hwnd, 0.5f, 0.5f);
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

bool isRed(const Rgb& pixel)
{
    return pixel.r > 200 && pixel.g >= 0 && pixel.g < 60 && pixel.b >= 0
           && pixel.b < 60;
}

// DWM picks the committed visual tree up asynchronously, so poll: the deadline
// only matters when the backend is broken.
template <typename Predicate>
bool composedWithin(Predicate composed, int attempts = 60)
{
    for (auto attempt = 0; attempt < attempts; ++attempt)
    {
        pumpPendingMessages();

        if (composed())
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return false;
}

void report(const char* where, const Rgb& pixel)
{
    std::cout << where << " pixel: r=" << pixel.r << " g=" << pixel.g
              << " b=" << pixel.b << "\n";
}

// A real window for the length of a case, in a suite that runs headless.
struct OnScreen
{
    OnScreen() { Apps::getAppEnvironment().headless = false; }
    ~OnScreen() { Apps::getAppEnvironment().headless = previous; }

    bool previous = Apps::getAppEnvironment().headless;
};

// Red underneath, and a green view added on top of it while hidden.
struct HiddenViewOnTop final : View
{
    HiddenViewOnTop()
    {
        addSubview(background);
        hidden.setVisible(false);
        addSubview(hidden);
    }

    void resized() override
    {
        background.setBounds(getLocalBounds());
        hidden.setBounds(getLocalBounds());
    }

    FilledView background {{1.f, 0.f, 0.f}};
    FilledView hidden {{0.f, 1.f, 0.f}};
};

// Red underneath, and a green view on top of it.
struct GreenOnRed final : View
{
    GreenOnRed() { addChildren({background, removed}); }

    void resized() override
    {
        background.setBounds(getLocalBounds());
        removed.setBounds(getLocalBounds());
    }

    FilledView background {{1.f, 0.f, 0.f}};
    FilledView removed {{0.f, 1.f, 0.f}};
};

// Two red halves. A green view is hidden in the left one, moved to the right
// one while hidden, and shown there.
struct HiddenViewMoved final : View
{
    HiddenViewMoved()
    {
        addChildren({left, right});

        left.addSubview(moved);
        moved.setVisible(false);
        left.removeSubview(moved);
        right.addSubview(moved);
        moved.setVisible(true);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        left.setBounds({0.f, 0.f, bounds.w / 2.f, bounds.h});
        right.setBounds({bounds.w / 2.f, 0.f, bounds.w / 2.f, bounds.h});
        moved.setBounds(right.getLocalBounds());
    }

    FilledView left {{1.f, 0.f, 0.f}};
    FilledView right {{1.f, 0.f, 0.f}};
    FilledView moved {{0.f, 1.f, 0.f}};
};
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

// DirectComposition has no hidden flag, so a hidden view is one kept out of
// the visual tree. Adding it to a parent must not put it in.
auto tHiddenViewStaysHidden =
    test("Composition/aHiddenViewAddedToAParentStaysHidden") = []
{
    if (!isCompositorInitialized())
    {
        check(true);
        return;
    }

    auto onScreen = OnScreen {};
    auto options = WindowOptions {};
    options.isPrimary = false;

    auto window = Window {options};
    auto content = HiddenViewOnTop {};
    window.setContentView(content);

    auto hwnd = static_cast<HWND>(window.getHandle());
    auto pixel = Rgb {};

    auto showsTheBackground = composedWithin(
        [&]
        {
            pixel = clientCenterPixel(hwnd);
            return isRed(pixel);
        });

    if (!showsTheBackground)
        report("center", pixel);

    check(showsTheBackground);

    auto turnedGreen = composedWithin(
        [&]
        {
            pixel = clientCenterPixel(hwnd);
            return isGreen(pixel);
        },
        10);

    if (turnedGreen)
        report("center", pixel);

    check(!turnedGreen);
    check(!content.hidden.isVisible());
};

// A hidden view forgets the parent it was hidden in when it is removed, so
// showing it afterwards does not put it back in that parent's visual tree.
auto tRemovedHiddenViewStaysOut =
    test("Composition/aHiddenViewRemovedThenShownStaysOutOfItsOldParent") = []
{
    if (!isCompositorInitialized())
    {
        check(true);
        return;
    }

    auto onScreen = OnScreen {};
    auto options = WindowOptions {};
    options.isPrimary = false;

    auto window = Window {options};
    auto content = GreenOnRed {};
    window.setContentView(content);

    auto hwnd = static_cast<HWND>(window.getHandle());
    auto pixel = Rgb {};
    auto sample = [&]
    {
        pixel = clientCenterPixel(hwnd);
        return pixel;
    };

    // Composited once first, so its layer has pixels to show if it comes back.
    check(composedWithin([&] { return isGreen(sample()); }));

    content.removed.setVisible(false);
    check(composedWithin([&] { return isRed(sample()); }));

    content.removeSubview(content.removed);
    content.removed.setVisible(true);

    auto cameBack = composedWithin([&] { return isGreen(sample()); }, 20);

    if (cameBack)
        report("center", pixel);

    check(!cameBack);
    check(content.removed.getParent() == nullptr);
};

// And showing it after a move puts it in its new parent.
auto tHiddenViewShowsInItsNewParent =
    test("Composition/aHiddenViewMovedToAnotherParentShowsThere") = []
{
    if (!isCompositorInitialized())
    {
        check(true);
        return;
    }

    auto onScreen = OnScreen {};
    auto options = WindowOptions {};
    options.isPrimary = false;

    auto window = Window {options};
    auto content = HiddenViewMoved {};
    window.setContentView(content);

    auto hwnd = static_cast<HWND>(window.getHandle());
    auto leftPixel = Rgb {};
    auto rightPixel = Rgb {};

    auto composed = composedWithin(
        [&]
        {
            leftPixel = clientPixelAt(hwnd, 0.25f, 0.5f);
            rightPixel = clientPixelAt(hwnd, 0.75f, 0.5f);
            return isRed(leftPixel) && isGreen(rightPixel);
        });

    if (!composed)
    {
        report("left", leftPixel);
        report("right", rightPixel);
    }

    check(composed);
};
