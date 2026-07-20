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
} // namespace eacp::GPU
