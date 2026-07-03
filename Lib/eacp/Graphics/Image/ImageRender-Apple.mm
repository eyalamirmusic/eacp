#import <CoreGraphics/CoreGraphics.h>

#include "ImageRender.h"
#include "ImageCodec.h"
#include "../Graphics/GraphicsContextImpl.h"

#include <eacp/Core/ObjC/CFRef.h>

namespace eacp::Graphics
{

Image renderToImage(int width,
                    int height,
                    const std::function<void(Context&)>& draw)
{
    if (width <= 0 || height <= 0)
        return {};

    auto rgba = ImageData {};
    rgba.resize(width * height * 4);

    auto colorSpace = CFRef<CGColorSpaceRef>(CGColorSpaceCreateDeviceRGB());
    auto bitmapInfo = static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast)
                      | static_cast<std::uint32_t>(kCGBitmapByteOrder32Big);

    auto cgContext = CFRef<CGContextRef>(
        CGBitmapContextCreate(rgba.data(),
                              static_cast<std::size_t>(width),
                              static_cast<std::size_t>(height),
                              8,
                              static_cast<std::size_t>(width) * 4,
                              colorSpace,
                              bitmapInfo));
    if (!cgContext)
        return {};

    // Flip to the top-left origin View::paint draws in.
    CGContextTranslateCTM(cgContext, 0, height);
    CGContextScaleCTM(cgContext, 1, -1);

    {
        auto context = MacOSContext(cgContext);
        draw(context);
    }

    detail::unpremultiply(rgba);
    return Image(width, height, std::move(rgba));
}

} // namespace eacp::Graphics
