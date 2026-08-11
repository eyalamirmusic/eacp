#include "ImageConversion-macOS.h"

namespace eacp::Graphics
{

NSImage* toNSImage(const Image& image)
{
    auto width = image.width();
    auto height = image.height();

    if (width <= 0 || height <= 0)
        return nil;

    // Image is straight-alpha top-left-origin RGBA, which is exactly an
    // NSBitmapImageRep with the non-premultiplied flag.
    auto* rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nullptr
                      pixelsWide:width
                      pixelsHigh:height
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                    bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                     bytesPerRow:width * 4
                    bitsPerPixel:32];

    std::memcpy([rep bitmapData],
                image.pixels().data(),
                static_cast<std::size_t>(width) * height * 4);

    auto* nsImage = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [nsImage addRepresentation:rep];
    [rep release];

    return [nsImage autorelease];
}

} // namespace eacp::Graphics
