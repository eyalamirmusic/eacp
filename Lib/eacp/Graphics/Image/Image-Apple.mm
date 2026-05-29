#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "ImageCodec.h"

#include <eacp/Core/ObjC/CFRef.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace eacp::Graphics::detail
{
namespace
{
CFRef<CGColorSpaceRef> deviceRGB()
{
    return {CGColorSpaceCreateDeviceRGB()};
}

// CGBitmapContext only produces premultiplied alpha. Straighten it so
// the stored RGBA matches what PNG/WIC consider canonical. Opaque and
// fully transparent pixels are left untouched.
void unpremultiply(ImageData& rgba)
{
    auto count = rgba.size();
    auto* p = rgba.data();
    for (auto i = 0; i + 3 < count; i += 4)
    {
        auto a = p[i + 3];
        if (a == 0 || a == 255)
            continue;

        for (auto c = 0; c < 3; ++c)
        {
            auto straight = (static_cast<int>(p[i + c]) * 255 + a / 2) / a;
            p[i + c] = static_cast<std::uint8_t>(std::min(straight, 255));
        }
    }
}

// Fast, lossless path: when the decoded image is already 8-bit straight
// RGBA (or RGBX) in R,G,B,A byte order, copy its pixels straight out of
// the data provider instead of redrawing through a premultiplied bitmap
// context (which quantizes non-opaque pixels). Row padding is stripped.
// Returns false when the layout needs conversion, leaving the slow path
// to handle it.
bool extractStraightRGBA(CGImageRef image, int width, int height, ImageData& out)
{
    if (CGImageGetBitsPerComponent(image) != 8
        || CGImageGetBitsPerPixel(image) != 32)
        return false;

    auto info = CGImageGetBitmapInfo(image);
    auto order = info & kCGBitmapByteOrderMask;
    if (order != kCGBitmapByteOrderDefault && order != kCGBitmapByteOrder32Big)
        return false;

    auto alpha = CGImageGetAlphaInfo(image);
    if (alpha != kCGImageAlphaLast && alpha != kCGImageAlphaNoneSkipLast)
        return false;

    auto* provider = CGImageGetDataProvider(image);
    if (provider == nullptr)
        return false;

    auto pixelData = CFRef<CFDataRef>(CGDataProviderCopyData(provider));
    if (!pixelData)
        return false;

    // Stride and length come from Core Graphics as size_t / CFIndex.
    auto sourceStride = CGImageGetBytesPerRow(image);
    auto available = CFDataGetLength(pixelData);
    auto rowBytes = width * 4;
    if (sourceStride < static_cast<std::size_t>(rowBytes)
        || available < static_cast<CFIndex>(sourceStride) * height)
        return false;

    auto* bytes = CFDataGetBytePtr(pixelData);
    out.resize(rowBytes * height);
    auto* dst = out.data();
    for (auto y = 0; y < height; ++y)
        std::memcpy(dst + y * rowBytes,
                    bytes + static_cast<std::size_t>(y) * sourceStride,
                    rowBytes);

    // RGBX: the skipped channel is undefined, so force fully opaque.
    if (alpha == kCGImageAlphaNoneSkipLast)
    {
        auto count = out.size();
        for (auto i = 3; i < count; i += 4)
            dst[i] = 255;
    }

    return true;
}
} // namespace

Image decodeImageBytes(const std::uint8_t* data, int size, std::string& error)
{
    if (data == nullptr || size <= 0)
    {
        error = "empty image data";
        return {};
    }

    auto cfData = CFRef<CFDataRef>(
        CFDataCreate(nullptr, data, static_cast<CFIndex>(size)));
    if (!cfData)
    {
        error = "could not wrap image bytes";
        return {};
    }

    auto source = CFRef<CGImageSourceRef>(
        CGImageSourceCreateWithData(cfData, nullptr));
    if (!source)
    {
        error = "unrecognized image format";
        return {};
    }

    auto image = CFRef<CGImageRef>(
        CGImageSourceCreateImageAtIndex(source, 0, nullptr));
    if (!image)
    {
        error = "could not decode image";
        return {};
    }

    auto width = static_cast<int>(CGImageGetWidth(image));
    auto height = static_cast<int>(CGImageGetHeight(image));
    if (width <= 0 || height <= 0)
    {
        error = "decoded image has zero dimensions";
        return {};
    }

    constexpr auto maxPixels = std::numeric_limits<int>::max() / 4;
    if (height > maxPixels / width)
    {
        error = "decoded image is too large";
        return {};
    }

    auto rgba = ImageData {};
    if (extractStraightRGBA(image, width, height, rgba))
        return Image(width, height, std::move(rgba));

    // Slow path: redraw through a premultiplied RGBA context and undo
    // the premultiplication. Handles any source layout (CMYK, grayscale,
    // 16-bit, premultiplied) at the cost of 8-bit precision on alpha.
    rgba.resize(width * height * 4);

    auto colorSpace = deviceRGB();
    auto bitmapInfo = static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast)
                      | static_cast<std::uint32_t>(kCGBitmapByteOrder32Big);

    // Core Graphics bitmap dimensions/stride are size_t by API contract.
    auto context = CFRef<CGContextRef>(
        CGBitmapContextCreate(rgba.data(),
                              static_cast<std::size_t>(width),
                              static_cast<std::size_t>(height),
                              8,
                              static_cast<std::size_t>(width) * 4,
                              colorSpace,
                              bitmapInfo));
    if (!context)
    {
        error = "could not create RGBA bitmap context";
        return {};
    }

    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);

    unpremultiply(rgba);
    return Image(width, height, std::move(rgba));
}

ImageData encodeImageBytes(const std::uint8_t* rgba,
                           int width,
                           int height,
                           ImageFormat format,
                           float quality,
                           std::string& error)
{
    auto byteCount = static_cast<std::size_t>(width)
                     * static_cast<std::size_t>(height) * 4;

    // CGDataProviderCreateWithData does not copy; the buffer must stay
    // alive until the CGImage is finalized, which all happens below.
    auto provider = CFRef<CGDataProviderRef>(
        CGDataProviderCreateWithData(nullptr, rgba, byteCount, nullptr));
    if (!provider)
    {
        error = "could not wrap pixel buffer";
        return {};
    }

    auto colorSpace = deviceRGB();
    auto bitmapInfo = static_cast<std::uint32_t>(kCGImageAlphaLast)
                      | static_cast<std::uint32_t>(kCGBitmapByteOrder32Big);

    auto image = CFRef<CGImageRef>(
        CGImageCreate(static_cast<std::size_t>(width),
                      static_cast<std::size_t>(height),
                      8,
                      32,
                      static_cast<std::size_t>(width) * 4,
                      colorSpace,
                      bitmapInfo,
                      provider,
                      nullptr,
                      false,
                      kCGRenderingIntentDefault));
    if (!image)
    {
        error = "could not build CGImage from pixels";
        return {};
    }

    auto destData = CFRef<CFMutableDataRef>(CFDataCreateMutable(nullptr, 0));
    if (!destData)
    {
        error = "could not allocate output buffer";
        return {};
    }

    auto type = format == ImageFormat::png ? CFSTR("public.png")
                                           : CFSTR("public.jpeg");
    auto destination = CFRef<CGImageDestinationRef>(
        CGImageDestinationCreateWithData(destData, type, 1, nullptr));
    if (!destination)
    {
        error = "could not create image destination";
        return {};
    }

    auto properties = CFRef<CFDictionaryRef>();
    if (format == ImageFormat::jpeg)
    {
        auto clamped = static_cast<CGFloat>(std::clamp(quality, 0.f, 1.f));
        auto number = CFRef<CFNumberRef>(
            CFNumberCreate(nullptr, kCFNumberCGFloatType, &clamped));
        const void* keys[] = {kCGImageDestinationLossyCompressionQuality};
        const void* values[] = {number.get()};
        properties.reset(CFDictionaryCreate(nullptr,
                                            keys,
                                            values,
                                            1,
                                            &kCFTypeDictionaryKeyCallBacks,
                                            &kCFTypeDictionaryValueCallBacks));
    }

    CGImageDestinationAddImage(destination, image, properties.get());
    if (!CGImageDestinationFinalize(destination))
    {
        error = "image encoding failed";
        return {};
    }

    auto* bytes = CFDataGetBytePtr(destData);
    auto length = static_cast<std::size_t>(CFDataGetLength(destData));
    auto result = ImageData {};
    result.assign(bytes, bytes + length);
    return result;
}

} // namespace eacp::Graphics::detail
