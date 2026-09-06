#include "Texture.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

// Linux/Vulkan placeholder. Stage 3 of the Linux plan replaces this whole file
// with the real thing: VkImage + VkImageView through VMA, format translation,
// cube-as-array, supplied mip chains, block-compressed uploads, the
// multisampled companion and its resolve, the depth companion and its aspect
// masks, region update and read, and layout tracking.
//
// Until then a Texture on Linux has nothing behind it and says so. Every
// accessor reports the empty answer and isValid() is false, which is exactly
// what the rest of the module already handles: Device::makeTexture is allowed
// to fail on every backend (a format the device refuses, a zero-sized
// descriptor), a compute pass drops an invalid texture rather than binding it,
// and a kernel that declares one is refused a pipeline outright so nothing
// dispatches against a descriptor that was never written.
//
// What it is *not* is a Texture that reports itself valid with no image under
// it. That would turn every stage-3 gap into a wrong picture or a driver hang
// instead of a "no" a caller can act on.

namespace eacp::GPU
{
struct Texture::Native
{
    Native(Device&, const TextureDescriptor&, const void*) {}
    Native(Device&, void*) {}
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

void Texture::update(const void*, std::size_t) {}

void Texture::update(const Graphics::Rect&, const void*, std::size_t) {}

void Texture::read(void*, std::size_t) const {}

void Texture::read(const Graphics::Rect&, void*, std::size_t) const {}

int Texture::width() const
{
    return 0;
}

int Texture::height() const
{
    return 0;
}

bool Texture::isValid() const
{
    return false;
}

int Texture::mipLevels() const
{
    return 0;
}

bool Texture::isRenderTarget() const
{
    return false;
}

bool Texture::isCube() const
{
    return false;
}

bool Texture::isComputeWritable() const
{
    return false;
}

bool Texture::hasDepth() const
{
    return false;
}

bool Texture::hasStencil() const
{
    return false;
}

bool Texture::hasSampleableDepth() const
{
    return false;
}

int Texture::sampleCount() const
{
    return 1;
}

void* Texture::nativeTexture() const
{
    return nullptr;
}

void* Texture::nativeReadView() const
{
    return nullptr;
}

void* Texture::nativeDepthTexture() const
{
    return nullptr;
}

void* Texture::nativeMultisampleTexture() const
{
    return nullptr;
}

void* Texture::nativeResolvedDepthTexture() const
{
    return nullptr;
}
} // namespace eacp::GPU
