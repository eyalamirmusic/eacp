#include <eacp/GPU/Frame/RenderPassRegions.h>

#include <NanoTest/NanoTest.h>

#include <cstdint>
#include <limits>

// The device-free decisions behind RenderPass's scissor, viewport and buffer
// offset checks, so they are tested on every lane with or without a GPU.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace RenderPassRegionsTests
{
namespace
{
constexpr auto targetWidth = 40;
constexpr auto targetHeight = 30;

const auto target = makeTargetRectangle(targetWidth, targetHeight);

bool isSameRectangle(const Geometry::Rectangle<int>& a,
                     const Geometry::Rectangle<int>& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool isViewportKept(const Graphics::Rect& rect)
{
    return target.contains(rect) && !rect.isEmpty();
}
} // namespace

auto tTargetArea = test("RenderPassRegions/targetNeedsPositiveWidthAndHeight") = []
{
    check(!target.isEmpty());
    check(makeTargetRectangle(0, targetHeight).isEmpty());
    check(makeTargetRectangle(targetWidth, 0).isEmpty());
    check(makeTargetRectangle(-1, targetHeight).isEmpty());
};

auto tScissorRoundsOutward = test("RenderPassRegions/scissorRoundsOutward") = []
{
    const auto pixels =
        calculateScissorRectangle({2.5f, 3.25f, 10.0f, 5.5f}, target);
    check(isSameRectangle(pixels, {2, 3, 11, 6}));
};

auto tScissorWholePixels = test("RenderPassRegions/scissorKeepsWholePixels") = []
{
    const auto pixels = calculateScissorRectangle({4.0f, 6.0f, 8.0f, 2.0f}, target);
    check(isSameRectangle(pixels, {4, 6, 8, 2}));
};

auto tScissorClamps = test("RenderPassRegions/scissorClampsToTarget") = []
{
    const auto pixels =
        calculateScissorRectangle({-5.0f, -2.0f, 100.0f, 100.0f}, target);
    check(isSameRectangle(pixels, {0, 0, targetWidth, targetHeight}));
};

auto tScissorOutside = test("RenderPassRegions/scissorPastTargetIsEmpty") = []
{
    const auto pixels =
        calculateScissorRectangle({50.0f, 40.0f, 10.0f, 10.0f}, target);
    check(isSameRectangle(pixels, {targetWidth, targetHeight, 0, 0}));
};

auto tScissorNegativeSize = test("RenderPassRegions/scissorNeverInverts") = []
{
    const auto pixels =
        calculateScissorRectangle({10.0f, 10.0f, -5.0f, -5.0f}, target);
    check(isSameRectangle(pixels, {10, 10, 0, 0}));
};

auto tViewportInside = test("RenderPassRegions/viewportInsideTargetIsKept") = []
{
    check(isViewportKept({0.0f, 0.0f, 40.0f, 30.0f}));
    check(isViewportKept({1.5f, 2.5f, 10.25f, 4.0f}));
};

auto tViewportRefused = test("RenderPassRegions/viewportOutsideTargetIsRefused") = []
{
    check(!isViewportKept({0.0f, 0.0f, 0.0f, 10.0f}));
    check(!isViewportKept({0.0f, 0.0f, 10.0f, -1.0f}));
    check(!isViewportKept({-1.0f, 0.0f, 10.0f, 10.0f}));
    check(!isViewportKept({0.0f, -1.0f, 10.0f, 10.0f}));
    check(!isViewportKept({30.5f, 0.0f, 10.0f, 10.0f}));
    check(!isViewportKept({0.0f, 20.5f, 10.0f, 10.0f}));
};

auto tViewportNan = test("RenderPassRegions/viewportWithNanEdgeIsRefused") = []
{
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    check(!isViewportKept({nan, 0.0f, 10.0f, 10.0f}));
};

auto tOffsetInside = test("RenderPassRegions/offsetMustLieInsideBuffer") = []
{
    constexpr auto bufferSize = std::uint64_t {64};
    check(isOffsetInsideBuffer(0, bufferSize));
    check(isOffsetInsideBuffer(63, bufferSize));
    check(!isOffsetInsideBuffer(64, bufferSize));
    check(!isOffsetInsideBuffer(-1, bufferSize));
    check(!isOffsetInsideBuffer(0, 0));
};
} // namespace RenderPassRegionsTests
