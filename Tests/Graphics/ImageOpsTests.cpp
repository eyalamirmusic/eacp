#include "Common.h"

using namespace nano;
using eacp::Graphics::Image;
using eacp::Graphics::ImageData;
using eacp::Graphics::mirroredCrop;
using eacp::Graphics::resizeBilinear;
using eacp::Graphics::warpAffineInverse;

namespace
{
Image makePattern(int width, int height)
{
    auto rgba = ImageData {};
    rgba.reserve(width * height * 4);
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
        {
            rgba.add(static_cast<std::uint8_t>(x * 7 + 1));
            rgba.add(static_cast<std::uint8_t>(y * 11 + 2));
            rgba.add(static_cast<std::uint8_t>((x + y) * 5 + 3));
            rgba.add(static_cast<std::uint8_t>(255 - x * 3));
        }
    return Image(width, height, std::move(rgba));
}

Image fromBytes(int width, int height, std::initializer_list<std::uint8_t> bytes)
{
    auto rgba = ImageData {};
    rgba.reserve(static_cast<int>(bytes.size()));
    for (auto b: bytes)
        rgba.add(b);
    return Image(width, height, std::move(rgba));
}
} // namespace

auto tResizeSameSizeReturnsEqual = test("ImageOps/resizeToSameSizeIsIdentity") = []
{
    auto original = makePattern(12, 7);
    auto resized = resizeBilinear(original, 12, 7);
    check(resized == original);
};

auto tResizeUpscaleOfSinglePixel =
    test("ImageOps/upscaleOfSinglePixelIsConstant") = []
{
    auto pixel = fromBytes(1, 1, {200, 150, 100, 255});
    auto resized = resizeBilinear(pixel, 4, 3);

    check(resized.width() == 4 && resized.height() == 3);
    const auto* p = resized.pixels().data();
    for (int i = 0; i < 4 * 3; ++i)
    {
        check(p[i * 4 + 0] == 200);
        check(p[i * 4 + 1] == 150);
        check(p[i * 4 + 2] == 100);
        check(p[i * 4 + 3] == 255);
    }
};

auto tResizeDownscaleAverages = test("ImageOps/downscale2x2To1x1Averages") = []
{
    // Half-pixel centers put the single output sample dead-centre, so each of
    // the four source pixels gets weight 0.25 -- the output is their average.
    auto src = fromBytes(
        2,
        2,
        {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160});
    auto resized = resizeBilinear(src, 1, 1);

    check(resized.width() == 1 && resized.height() == 1);
    const auto* p = resized.pixels().data();
    check(p[0] == 70); // (10+50+90+130)/4
    check(p[1] == 80); // (20+60+100+140)/4
    check(p[2] == 90); // (30+70+110+150)/4
    check(p[3] == 100); // (40+80+120+160)/4
};

auto tResizeInvalidInputsReturnInvalid =
    test("ImageOps/resizeInvalidInputsReturnInvalid") = []
{
    auto valid = makePattern(4, 4);
    check(!resizeBilinear(valid, 0, 4));
    check(!resizeBilinear(valid, 4, -1));
    check(!resizeBilinear(Image {}, 4, 4));
};

auto tWarpIdentityReturnsSource = test("ImageOps/warpIdentityReturnsSource") = []
{
    // The default Affine2x3 is the identity inverse, so each dst pixel samples
    // its own source pixel -- a same-size warp must return the source untouched.
    auto original = makePattern(12, 7);
    auto warped = warpAffineInverse(original, eacp::Graphics::Affine2x3 {}, 12, 7);
    check(warped == original);
};

auto tWarpInvalidInputsReturnInvalid =
    test("ImageOps/warpInvalidInputsReturnInvalid") = []
{
    auto valid = makePattern(4, 4);
    const auto identity = eacp::Graphics::Affine2x3 {};
    check(!warpAffineInverse(valid, identity, 0, 4));
    check(!warpAffineInverse(valid, identity, 4, -1));
    check(!warpAffineInverse(Image {}, identity, 4, 4));
};

auto tMirroredCropFlipsHorizontally =
    test("ImageOps/mirroredCropFlipsHorizontally") = []
{
    // Four distinct pixels in a row; a full-width mirrored crop reverses them.
    auto src = fromBytes(
        4, 1, {10, 11, 12, 13, 20, 21, 22, 23, 30, 31, 32, 33, 40, 41, 42, 43});
    auto cropped = mirroredCrop(src, 0, 0, 4, 1);

    check(cropped.width() == 4 && cropped.height() == 1);
    const auto* p = cropped.pixels().data();
    check(p[0] == 40 && p[1] == 41 && p[2] == 42 && p[3] == 43);
    check(p[4] == 30 && p[8] == 20 && p[12] == 10);
};

auto tMirroredCropRejectsOutOfBounds =
    test("ImageOps/mirroredCropRejectsOutOfBounds") = []
{
    auto src = makePattern(8, 8);
    check(!mirroredCrop(src, 0, 0, 0, 4)); // zero width
    check(!mirroredCrop(src, 6, 0, 4, 4)); // x + width > srcW
    check(!mirroredCrop(src, -1, 0, 2, 2)); // negative x
    check(!mirroredCrop(Image {}, 0, 0, 2, 2)); // invalid source
};
