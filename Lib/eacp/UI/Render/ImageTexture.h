#pragma once

#include "../Common.h"

#include <eacp/GPU/GPU.h>

#include <cstdint>
#include <memory>

namespace eacp::UI
{
// A decoded image on the GPU: its texture, with the chain of smaller levels a
// picture drawn below its own size samples from, and the size it came in at.
//
// Shared rather than owned, because there is no one place in a component tree
// an image belongs. A recording that draws it holds a reference, so a list
// replayed after the component that painted it has let the image go still
// draws a texture that exists; the cache that uploaded it holds one too, and
// drops it once nothing else does. See ImageCache, which is the only thing
// that makes one.
struct ImageTexture
{
    ImageTexture(const GPU::TextureDescriptor& descriptor,
                 const void* pixels,
                 std::uint64_t hashToUse)
        : texture(GPU::Device::shared(), descriptor, pixels)
        , width(descriptor.width)
        , height(descriptor.height)
        , hash(hashToUse)
    {
    }

    GPU::Texture texture;
    int width = 0;
    int height = 0;

    // Of the pixels it was made from, which is how the cache finds it again.
    std::uint64_t hash = 0;
};

using ImageRef = std::shared_ptr<const ImageTexture>;
} // namespace eacp::UI
