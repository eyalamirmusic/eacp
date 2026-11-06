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

bool isScissor(const ScissorPixels& pixels, const ScissorPixels& expected)
{
    return pixels.left == expected.left && pixels.top == expected.top
           && pixels.right == expected.right && pixels.bottom == expected.bottom;
}
} // namespace

auto tTargetArea = test("RenderPassRegions/targetNeedsPositiveWidthAndHeight") = []
{
    check(hasTargetArea(targetWidth, targetHeight));
    check(!hasTargetArea(0, targetHeight));
    check(!hasTargetArea(targetWidth, 0));
    check(!hasTargetArea(-1, targetHeight));
};

auto tScissorRoundsOutward = test("RenderPassRegions/scissorRoundsOutward") = []
{
    const auto pixels = calculateScissorPixels(
        {2.5f, 3.25f, 10.0f, 5.5f}, targetWidth, targetHeight);
    check(isScissor(pixels, {2, 3, 13, 9}));
};

auto tScissorWholePixels = test("RenderPassRegions/scissorKeepsWholePixels") = []
{
    const auto pixels =
        calculateScissorPixels({4.0f, 6.0f, 8.0f, 2.0f}, targetWidth, targetHeight);
    check(isScissor(pixels, {4, 6, 12, 8}));
};

auto tScissorClamps = test("RenderPassRegions/scissorClampsToTarget") = []
{
    const auto pixels = calculateScissorPixels(
        {-5.0f, -2.0f, 100.0f, 100.0f}, targetWidth, targetHeight);
    check(isScissor(pixels, {0, 0, targetWidth, targetHeight}));
};

auto tScissorOutside = test("RenderPassRegions/scissorPastTargetIsEmpty") = []
{
    const auto pixels = calculateScissorPixels(
        {50.0f, 40.0f, 10.0f, 10.0f}, targetWidth, targetHeight);
    check(isScissor(pixels, {targetWidth, targetHeight, targetWidth, targetHeight}));
};

auto tScissorNegativeSize = test("RenderPassRegions/scissorNeverInverts") = []
{
    const auto pixels = calculateScissorPixels(
        {10.0f, 10.0f, -5.0f, -5.0f}, targetWidth, targetHeight);
    check(isScissor(pixels, {10, 10, 10, 10}));
};

auto tViewportInside = test("RenderPassRegions/viewportInsideTargetIsKept") = []
{
    check(isViewportInsideTarget(
        {0.0f, 0.0f, 40.0f, 30.0f}, targetWidth, targetHeight));
    check(isViewportInsideTarget(
        {1.5f, 2.5f, 10.25f, 4.0f}, targetWidth, targetHeight));
};

auto tViewportRefused = test("RenderPassRegions/viewportOutsideTargetIsRefused") = []
{
    check(!isViewportInsideTarget(
        {0.0f, 0.0f, 0.0f, 10.0f}, targetWidth, targetHeight));
    check(!isViewportInsideTarget(
        {0.0f, 0.0f, 10.0f, -1.0f}, targetWidth, targetHeight));
    check(!isViewportInsideTarget(
        {-1.0f, 0.0f, 10.0f, 10.0f}, targetWidth, targetHeight));
    check(!isViewportInsideTarget(
        {0.0f, -1.0f, 10.0f, 10.0f}, targetWidth, targetHeight));
    check(!isViewportInsideTarget(
        {30.5f, 0.0f, 10.0f, 10.0f}, targetWidth, targetHeight));
    check(!isViewportInsideTarget(
        {0.0f, 20.5f, 10.0f, 10.0f}, targetWidth, targetHeight));
};

auto tViewportNan = test("RenderPassRegions/viewportWithNanEdgeIsKept") = []
{
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    check(isViewportInsideTarget(
        {nan, 0.0f, 10.0f, 10.0f}, targetWidth, targetHeight));
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
