#include "ImagePatterns.h"

#include <filesystem>

using namespace nano;
using eacp::Graphics::Image;
using eacp::Graphics::ImageData;
using ImagePatterns::makeOpaquePattern;

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

auto tSaveUnknownExtensionThrows = test("Image/saveUnknownExtensionThrows") = []
{
    auto image = makeOpaquePattern(2, 2);
    auto threw = false;
    try
    {
        image.save(std::filesystem::temp_directory_path() / "eacp-image-test.bmp");
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
    auto loaded = Image::load(std::filesystem::temp_directory_path()
                                  / "eacp-image-does-not-exist.png",
                              &error);
    check(!loaded);
    check(!error.empty());
};
