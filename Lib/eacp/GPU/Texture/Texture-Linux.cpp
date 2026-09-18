#include "Texture.h"

#include "../Device/Device.h"
#include "../Linux/GPUBackend-Linux.h"

#include <cmath>

namespace eacp::GPU
{
struct Texture::Native
{
    Native(Device& device, const TextureDescriptor& descriptor, const void* pixels)
        : backend(getDeviceBackend(device).makeTexture(device, descriptor, pixels))
    {
    }

    Native(Device& device, void* nativePixelBuffer)
        : backend(
              getDeviceBackend(device).wrapPixelBuffer(device, nativePixelBuffer))
    {
    }

    std::unique_ptr<TextureBackend> backend;
};

Texture::Texture(Device& device,
                 const TextureDescriptor& descriptor,
                 const void* pixels)
    : impl(device, descriptor, pixels)
{
}

Texture::Texture(Device& device, void* nativePixelBuffer)
    : impl(device, nativePixelBuffer)
{
}

void Texture::update(const void* pixels, int bytesPerRow)
{
    if (bytesPerRow < 0)
        return;

    impl->backend->update(pixels, bytesPerRow);
}

void Texture::update(const Graphics::Rect& region,
                     const void* pixels,
                     int bytesPerRow)
{
    if (bytesPerRow < 0)
        return;

    impl->backend->updateRegion(static_cast<int>(std::lround(region.x)),
                                static_cast<int>(std::lround(region.y)),
                                static_cast<int>(std::lround(region.w)),
                                static_cast<int>(std::lround(region.h)),
                                pixels,
                                bytesPerRow);
}

void Texture::read(void* dst, int bytesPerRow) const
{
    if (bytesPerRow < 0)
        return;

    impl->backend->readRegion(
        0, 0, impl->backend->width(), impl->backend->height(), dst, bytesPerRow);
}

void Texture::read(const Graphics::Rect& region, void* dst, int bytesPerRow) const
{
    if (bytesPerRow < 0)
        return;

    impl->backend->readRegion(static_cast<int>(std::lround(region.x)),
                              static_cast<int>(std::lround(region.y)),
                              static_cast<int>(std::lround(region.w)),
                              static_cast<int>(std::lround(region.h)),
                              dst,
                              bytesPerRow);
}

int Texture::width() const
{
    return impl->backend->width();
}

int Texture::height() const
{
    return impl->backend->height();
}

bool Texture::isValid() const
{
    return impl->backend->isValid();
}

int Texture::mipLevels() const
{
    return impl->backend->mipLevels();
}

bool Texture::isRenderTarget() const
{
    return impl->backend->isRenderTarget();
}

bool Texture::isCube() const
{
    return impl->backend->isCube();
}

bool Texture::isComputeWritable() const
{
    return impl->backend->isComputeWritable();
}

bool Texture::hasDepth() const
{
    return impl->backend->hasDepth();
}

bool Texture::hasStencil() const
{
    return impl->backend->hasStencil();
}

bool Texture::hasSampleableDepth() const
{
    return impl->backend->hasSampleableDepth();
}

int Texture::sampleCount() const
{
    return impl->backend->sampleCount();
}

void* Texture::nativeTexture() const
{
    return impl->backend->nativeTexture();
}

void* Texture::nativeReadView() const
{
    return impl->backend->nativeReadView();
}

void* Texture::nativeDepthTexture() const
{
    return impl->backend->nativeDepthTexture();
}

void* Texture::nativeMultisampleTexture() const
{
    return impl->backend->nativeMultisampleTexture();
}

void* Texture::nativeResolvedDepthTexture() const
{
    return impl->backend->nativeResolvedDepthTexture();
}
} // namespace eacp::GPU
