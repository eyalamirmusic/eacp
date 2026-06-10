#import <Metal/Metal.h>

#include "Texture.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
namespace
{
MTLPixelFormat toMetalFormat(TextureFormat format)
{
    return format == TextureFormat::BGRA8Unorm ? MTLPixelFormatBGRA8Unorm
                                               : MTLPixelFormatRGBA8Unorm;
}

MTLSamplerMinMagFilter toMetalFilter(TextureFilter filter)
{
    return filter == TextureFilter::Nearest ? MTLSamplerMinMagFilterNearest
                                            : MTLSamplerMinMagFilterLinear;
}

MTLSamplerAddressMode toMetalAddressMode(TextureAddressMode mode)
{
    return mode == TextureAddressMode::Repeat ? MTLSamplerAddressModeRepeat
                                              : MTLSamplerAddressModeClampToEdge;
}
} // namespace

struct Texture::Native
{
    Native(Device& device, const TextureDescriptor& descriptor, const void* pixels)
        : width(descriptor.width)
        , height(descriptor.height)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();

        if (metalDevice == nil || width <= 0 || height <= 0)
            return;

        auto textureDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:toMetalFormat(descriptor.format)
                                         width:(NSUInteger) width
                                        height:(NSUInteger) height
                                     mipmapped:NO];
        textureDescriptor.usage = MTLTextureUsageShaderRead;

        texture = [metalDevice newTextureWithDescriptor:textureDescriptor];

        // The default storage mode keeps replaceRegion valid on every Mac
        // generation; it handles the CPU-to-GPU synchronisation itself.
        if (texture.get() != nil && pixels != nullptr)
            [texture.get() replaceRegion:MTLRegionMake2D(0,
                                                         0,
                                                         (NSUInteger) width,
                                                         (NSUInteger) height)
                             mipmapLevel:0
                               withBytes:pixels
                             bytesPerRow:(NSUInteger) width * 4];

        auto samplerDescriptor = ObjC::makePtr<MTLSamplerDescriptor>();
        samplerDescriptor.get().minFilter = toMetalFilter(descriptor.filter);
        samplerDescriptor.get().magFilter = toMetalFilter(descriptor.filter);
        samplerDescriptor.get().sAddressMode =
            toMetalAddressMode(descriptor.addressMode);
        samplerDescriptor.get().tAddressMode =
            toMetalAddressMode(descriptor.addressMode);

        sampler = [metalDevice newSamplerStateWithDescriptor:samplerDescriptor.get()];
    }

    int width = 0;
    int height = 0;
    ObjC::Ptr<NSObject<MTLTexture>> texture;
    ObjC::Ptr<NSObject<MTLSamplerState>> sampler;
};

Texture::Texture(Device& device,
                 const TextureDescriptor& descriptor,
                 const void* pixels)
    : impl(device, descriptor, pixels)
{
}

int Texture::width() const
{
    return impl->width;
}

int Texture::height() const
{
    return impl->height;
}

bool Texture::isValid() const
{
    return impl->texture.get() != nil && impl->sampler.get() != nil;
}

void* Texture::nativeTexture() const
{
    return (__bridge void*) impl->texture.get();
}

void* Texture::nativeSampler() const
{
    return (__bridge void*) impl->sampler.get();
}

void* Texture::nativeReadView() const
{
    return nullptr;
}
} // namespace eacp::GPU
