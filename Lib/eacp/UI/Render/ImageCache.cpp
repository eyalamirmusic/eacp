#include "ImageCache.h"

#include "ContentHash.h"

#include <cstring>

namespace eacp::UI
{
namespace
{
// Every pixel, a word at a time, with the dimensions in front -- so a wide
// image and a tall one holding the same bytes in a different shape are two
// keys.
std::uint64_t hashOf(const eacp::Graphics::Image& image)
{
    auto hash = ContentHash {};
    hash.mix(image.width());
    hash.mix(image.height());

    const auto* bytes = image.pixels().data();
    auto words = image.pixels().size() / 4;

    for (auto i = 0; i < words; ++i)
    {
        auto word = std::uint32_t {};
        std::memcpy(&word, bytes + i * 4, sizeof(word));
        hash.mix(word);
    }

    return hash.get();
}
} // namespace

ImageRef ImageCache::get(const eacp::Graphics::Image& image)
{
    if (!image.isValid() || image.width() <= 0 || image.height() <= 0)
        return nullptr;

    auto hash = hashOf(image);
    auto found = entries.find(hash);

    if (found != entries.end())
        return found->second;

    // With the chain, always: an image in an interface is drawn at whatever
    // size the layout gave it, which is below its own more often than not, and
    // a level near that size is what keeps a minified picture from shimmering.
    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = image.width();
    descriptor.height = image.height();
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;
    descriptor.mipmapped = true;

    auto entry =
        std::make_shared<ImageTexture>(descriptor, image.pixels().data(), hash);

    if (!entry->texture.isValid())
        return nullptr;

    entries.emplace(hash, entry);

    return entry;
}

void ImageCache::releaseUnused()
{
    for (auto it = entries.begin(); it != entries.end();)
    {
        if (it->second.use_count() == 1)
            it = entries.erase(it);
        else
            ++it;
    }
}
} // namespace eacp::UI
