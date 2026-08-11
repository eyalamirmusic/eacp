#include "Device.h"

namespace eacp::GPU
{
Texture Device::makeTexture(const Graphics::Image& image)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = image.width();
    descriptor.height = image.height();
    descriptor.format = TextureFormat::RGBA8Unorm;

    return makeTexture(descriptor, image.pixels().data());
}

void Device::beginFrame()
{
    ++frameCount;

    timer.beginFrame(frameCount);
}
} // namespace eacp::GPU
