#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "ImageCodec.h"

#include <eacp/Core/ObjC/CFRef.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

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
void unpremultiply(std::vector<std::uint8_t>& rgba)
{
    for (auto i = std::size_t {0}; i + 3 < rgba.size(); i += 4)
    {
        auto a = rgba[i + 3];
        if (a == 0 || a == 255)
            continue;

        for (auto c = std::size_t {0}; c < 3; ++c)
        {
            auto straight = (static_cast<int>(rgba[i + c]) * 255 + a / 2) / a;
            rgba[i + c] = static_cast<std::uint8_t>(std::min(straight, 255));
        }
    }
}

// Fast, lossless path: when the decoded image is already 8-bit straight
// RGBA (or RGBX) in R,G,B,A byte order, copy its pixels straight out of
// the data provider instead of redrawing through a premultiplied bitmap
// context (which quantizes non-opaque pixels). Row padding is stripped.
// Returns false when the layout needs conversion, leaving the slow path
// to handle it.
bool extractStraightRGBA(CGImageRef image,
                         int width,
                         int height,
                         std::vector<std::uint8_t>& out)
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

    auto sourceStride = CGImageGetBytesPerRow(image);
    auto rowBytes = static_cast<std::size_t>(width) * 4;
    auto available = static_cast<std::size_t>(CFDataGetLength(pixelData));
    if (sourceStride < rowBytes
        || available < sourceStride * static_cast<std::size_t>(height))
        return false;

    auto* bytes = CFDataGetBytePtr(pixelData);
    out.resize(rowBytes * static_cast<std::size_t>(height));
    for (auto y = std::size_t {0}; y < static_cast<std::size_t>(height); ++y)
        std::memcpy(out.data() + y * rowBytes, bytes + y * sourceStride, rowBytes);

    // RGBX: the skipped channel is undefined, so force fully opaque.
    if (alpha == kCGImageAlphaNoneSkipLast)
        for (auto i = std::size_t {3}; i < out.size(); i += 4)
            out[i] = 255;

    return true;
}
} // namespace

std::optional<DecodedImage> decodeImageBytes(const std::uint8_t* data,
                                             std::size_t size,
                                             std::string& error)
{
    if (data == nullptr || size == 0)
    {
        error = "empty image data";
        return std::nullopt;
    }

    auto cfData = CFRef<CFDataRef>(
        CFDataCreate(nullptr, data, static_cast<CFIndex>(size)));
    if (!cfData)
    {
        error = "could not wrap image bytes";
        return std::nullopt;
    }

    auto source = CFRef<CGImageSourceRef>(
        CGImageSourceCreateWithData(cfData, nullptr));
    if (!source)
    {
        error = "unrecognized image format";
        return std::nullopt;
    }

    auto image = CFRef<CGImageRef>(
        CGImageSourceCreateImageAtIndex(source, 0, nullptr));
    if (!image)
    {
        error = "could not decode image";
        return std::nullopt;
    }

    auto width = static_cast<int>(CGImageGetWidth(image));
    auto height = static_cast<int>(CGImageGetHeight(image));
    if (width <= 0 || height <= 0)
    {
        error = "decoded image has zero dimensions";
        return std::nullopt;
    }

    auto decoded = DecodedImage {};
    decoded.width = width;
    decoded.height = height;

    if (extractStraightRGBA(image, width, height, decoded.rgba))
        return decoded;

    // Slow path: redraw through a premultiplied RGBA context and undo
    // the premultiplication. Handles any source layout (CMYK, grayscale,
    // 16-bit, premultiplied) at the cost of 8-bit precision on alpha.
    decoded.rgba.assign(static_cast<std::size_t>(width)
                            * static_cast<std::size_t>(height) * 4,
                        0);

    auto colorSpace = deviceRGB();
    auto bitmapInfo = static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast)
                      | static_cast<std::uint32_t>(kCGBitmapByteOrder32Big);

    auto context = CFRef<CGContextRef>(
        CGBitmapContextCreate(decoded.rgba.data(),
                              static_cast<std::size_t>(width),
                              static_cast<std::size_t>(height),
                              8,
                              static_cast<std::size_t>(width) * 4,
                              colorSpace,
                              bitmapInfo));
    if (!context)
    {
        error = "could not create RGBA bitmap context";
        return std::nullopt;
    }

    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);

    unpremultiply(decoded.rgba);
    return decoded;
}

std::vector<std::uint8_t> encodeImageBytes(const std::uint8_t* rgba,
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

    auto bytes = CFDataGetBytePtr(destData);
    auto length = static_cast<std::size_t>(CFDataGetLength(destData));
    return std::vector<std::uint8_t>(bytes, bytes + length);
}

} // namespace eacp::Graphics::detail
