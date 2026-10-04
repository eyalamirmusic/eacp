#include "ImagePatterns.h"

#include <filesystem>

using namespace nano;
using eacp::Graphics::Image;
using eacp::Graphics::ImageFormat;
using ImagePatterns::makeOpaquePattern;
using ImagePatterns::makeTranslucentPattern;

namespace
{
std::filesystem::path tempPath(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}
} // namespace

auto tPngRoundTripLossless = test("Image/pngRoundTripIsLossless") = []
{
    auto original = makeOpaquePattern(8, 5);

    auto png = original.toPng();
    check(!png.empty());

    auto error = std::string {};
    auto decoded = Image::decode(png, &error);
    check(static_cast<bool>(decoded));
    check(error.empty());
    check(decoded == original);
};

auto tPngRoundTripPreservesAlpha = test("Image/pngRoundTripPreservesAlpha") = []
{
    auto original = makeTranslucentPattern(9, 7);

    auto decoded = Image::decode(original.toPng());
    check(static_cast<bool>(decoded));
    // PNG is lossless and decode must keep straight (non-premultiplied)
    // alpha, so the bytes survive exactly even for partially transparent
    // pixels.
    check(decoded == original);
};

auto tEncodeFormatDetected = test("Image/decodeAutoDetectsPngAndJpeg") = []
{
    auto image = makeOpaquePattern(6, 6);

    auto png = image.encode(ImageFormat::png);
    auto jpeg = image.encode(ImageFormat::jpeg, 0.85f);

    auto fromPng = Image::decode(png);
    auto fromJpeg = Image::decode(jpeg);

    check(static_cast<bool>(fromPng));
    check(static_cast<bool>(fromJpeg));
    check(fromPng.width() == 6 && fromPng.height() == 6);
    check(fromJpeg.width() == 6 && fromJpeg.height() == 6);
};

auto tJpegPreservesDimensions = test("Image/jpegRoundTripPreservesDimensions") = []
{
    auto original = makeOpaquePattern(16, 9);

    auto decoded = Image::decode(original.toJpeg(0.9f));
    check(static_cast<bool>(decoded));
    check(decoded.isValid());
    check(decoded.width() == 16);
    check(decoded.height() == 9);
};

auto tSaveLoadPngRoundTrips = test("Image/saveAndLoadPngRoundTrips") = []
{
    auto original = makeOpaquePattern(10, 4);
    auto path = tempPath("eacp-image-test-roundtrip.png");

    original.save(path);
    check(std::filesystem::exists(path));

    auto loaded = Image::load(path);
    check(static_cast<bool>(loaded));
    check(loaded == original);

    std::filesystem::remove(path);
};

auto tSaveInfersJpegFromExtension = test("Image/saveInfersJpegFromExtension") = []
{
    auto original = makeOpaquePattern(12, 8);
    auto path = tempPath("eacp-image-test.jpg");

    original.save(path);
    check(std::filesystem::exists(path));

    auto loaded = Image::load(path);
    check(static_cast<bool>(loaded));
    check(loaded.width() == 12 && loaded.height() == 8);

    std::filesystem::remove(path);
};
