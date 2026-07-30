#include "Device.h"

// Portable Device members. The platform backends (Device-macOS.mm /
// Device-Windows.cpp) own construction and the native handles; anything that
// only builds on the public API lives here so it compiles once for every
// platform.

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

    // The timer takes its slot from the counter, the same way StreamingBuffers
    // takes its pool from it - one advance, driven by whoever built the Frame,
    // and nothing for either of them to be told separately.
    timer.beginFrame(frameCount);
}
} // namespace eacp::GPU
