#include "Common.h"

using namespace nano;
using eacp::Graphics::Image;
using eacp::Graphics::ImageData;
using eacp::Graphics::ImageFormat;

namespace
{
// Deterministic, fully opaque RGBA pattern. Opaque alpha keeps the PNG
// round-trip byte-exact (no premultiplied-alpha precision loss).
Image makeOpaquePattern(int width, int height)
{
    auto rgba = ImageData {};
    rgba.reserve(width * height * 4);
    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            rgba.add(static_cast<std::uint8_t>(x * 7 + 1));
            rgba.add(static_cast<std::uint8_t>(y * 11 + 2));
            rgba.add(static_cast<std::uint8_t>((x + y) * 5 + 3));
            rgba.add(static_cast<std::uint8_t>(255));
        }
    }
    return Image(width, height, std::move(rgba));
}

// Deterministic pattern with varying (non-opaque) alpha. Exercises the
// straight-alpha decode path, which must not quantize through a
// premultiplied bitmap context.
Image makeTranslucentPattern(int width, int height)
{
    auto rgba = ImageData {};
    rgba.reserve(width * height * 4);
    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            rgba.add(static_cast<std::uint8_t>(200 - x * 9));
            rgba.add(static_cast<std::uint8_t>(40 + y * 13));
            rgba.add(static_cast<std::uint8_t>(50 + (x + y) * 6));
            rgba.add(static_cast<std::uint8_t>(16 + x * 17 + y * 3));
        }
    }
    return Image(width, height, std::move(rgba));
}

std::filesystem::path tempPath(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}
} // namespace

auto tConstructsZeroFilled = test("Image/constructsZeroFilledAndTransparent") = []
{
    auto image = Image(4, 3);

    check(image.isValid());
    check(static_cast<bool>(image));
    check(image.width() == 4);
    check(image.height() == 3);
    check(image.pixels().size() == 4 * 3 * 4);
    check(image.at(0, 0).a == 0.f);
    check(image.at(3, 2).r == 0.f);
};

auto tDefaultImageIsInvalid = test("Image/defaultConstructedIsEmptyAndInvalid") = []
{
    auto image = Image {};

    check(image.isEmpty());
    check(!image.isValid());
    check(!image);
    check(image.width() == 0);
    check(image.height() == 0);
};

auto tSetAndAtRoundTrip = test("Image/setAndAtRoundTripChannels") = []
{
    auto image = Image(2, 2);
    auto color = eacp::Graphics::Color {0.2f, 0.4f, 0.6f, 1.f};
    image.set(1, 0, color);

    auto read = image.at(1, 0);
    check(std::abs(read.r - 0.2f) < 0.01f);
    check(std::abs(read.g - 0.4f) < 0.01f);
    check(std::abs(read.b - 0.6f) < 0.01f);
    check(std::abs(read.a - 1.f) < 0.01f);
    // Untouched neighbour stays transparent.
    check(image.at(0, 0).a == 0.f);
};

auto tOutOfRangeAccessIsSafe = test("Image/outOfRangeAccessIsSafe") = []
{
    auto image = Image(2, 2);

    check(image.at(-1, 0).a == 0.f);
    check(image.at(0, 5).a == 0.f);
    // Out-of-range write is ignored, not a crash or corruption.
    image.set(10, 10, eacp::Graphics::Color::white());
    check(image.pixels().size() == 2 * 2 * 4);
};

auto tInvalidPixelBufferThrows = test("Image/explicitBufferSizeMismatchThrows") = []
{
    auto threw = false;
    try
    {
        auto bad = Image(2, 2, ImageData(3));
        (void) bad;
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    check(threw);
};

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

auto tNegativeDimensionsThrow = test("Image/negativeDimensionsThrow") = []
{
    auto zeroFillThrew = false;
    try
    {
        auto bad = Image(-1, 4);
        (void) bad;
    }
    catch (const std::invalid_argument&)
    {
        zeroFillThrew = true;
    }
    check(zeroFillThrew);

    auto bufferThrew = false;
    try
    {
        auto bad = Image(-1, 4, ImageData {});
        (void) bad;
    }
    catch (const std::invalid_argument&)
    {
        bufferThrew = true;
    }
    check(bufferThrew);
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

auto tEqualitySemantics = test("Image/equalitySemantics") = []
{
    auto a = makeOpaquePattern(4, 4);
    auto b = makeOpaquePattern(4, 4);
    check(a == b);
    check(!(a != b));

    b.set(0, 0, eacp::Graphics::Color {0.9f, 0.1f, 0.1f, 1.f});
    check(a != b);

    auto differentSize = makeOpaquePattern(4, 5);
    check(a != differentSize);
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

auto tSaveUnknownExtensionThrows = test("Image/saveUnknownExtensionThrows") = []
{
    auto image = makeOpaquePattern(2, 2);
    auto threw = false;
    try
    {
        image.save(tempPath("eacp-image-test.bmp"));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    check(threw);
};

auto tDecodeGarbageReturnsInvalid = test("Image/decodeGarbageReturnsInvalid") = []
{
    const char garbage[] = "this is definitely not an image file";

    auto error = std::string {};
    auto decoded = Image::decode(reinterpret_cast<const std::uint8_t*>(garbage),
                                 static_cast<int>(sizeof(garbage) - 1),
                                 &error);
    check(!decoded);
    check(!error.empty());
};

auto tDecodeEmptyReturnsInvalid = test("Image/decodeEmptyReturnsInvalid") = []
{
    auto decoded = Image::decode(ImageData {});
    check(!decoded);
};

auto tLoadMissingFileReturnsInvalid =
    test("Image/loadMissingFileReturnsInvalid") = []
{
    auto error = std::string {};
    auto loaded = Image::load(tempPath("eacp-image-does-not-exist.png"), &error);
    check(!loaded);
    check(!error.empty());
};
