#include "ImageTexture.h"

namespace eacp::UI
{
ImageTexture::ImageTexture(const GPU::TextureDescriptor& descriptor,
                           const void* pixels,
                           std::uint64_t hashToUse)
    : texture(GPU::Device::shared(), descriptor, pixels)
    , width(descriptor.width)
    , height(descriptor.height)
    , hash(hashToUse)
{
}
} // namespace eacp::UI
