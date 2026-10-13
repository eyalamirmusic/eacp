#pragma once

#include "ImageTexture.h"

#include <unordered_map>

namespace eacp::UI
{
// Turns a decoded image into a texture once, however many components draw it.
//
// Keyed by content and by nothing else: two images with the same pixels are one
// texture whatever buffers they arrived in, so an icon decoded in forty places
// costs the GPU one upload, and a caller has nothing to name and nothing to get
// wrong. The key is a hash of every pixel. That is what makes the sharing safe,
// and it is also what it costs -- once per pixel buffer rather than once per
// ask, see get().
//
// Where the tier's other caches keep what they hashed and compare it on a hit
// -- a gradient's stops, a path's points -- this one cannot: the pixels are the
// thing the texture exists to replace, and keeping them would double every
// picture in the tree. So a match is trusted on 64 bits over every pixel and
// both dimensions, and the picture that would be wrong is one where two
// different images on one screen hash alike -- which is not a collision
// anything will meet.
//
// The cache holds a reference to everything it made, so an image asked for
// again is found rather than uploaded again; the host drops whatever nothing
// else holds after each recording walk. A caller that wants a texture kept
// across that keeps the reference, and that is the whole of the ownership rule.
class ImageCache
{
public:
    // The texture for `image`: uploaded on the first ask, with its mip chain,
    // and found by content on every later one. Null for an image with no
    // pixels, and for one the device refused -- wider or taller than a texture
    // can be.
    //
    // Every pixel is hashed the first time a buffer is seen, and the hash is
    // remembered against the buffer's identity (Image::pixelBuffer) and its
    // write count (Image::pixelGeneration). Asking again for the same image, or
    // for any copy sharing its pixels, finds that and reads no pixels at all; a
    // write in place moves the count and hashes the image again. The reference
    // is still the handle, and holding it is what keeps the texture alive
    // across frames.
    ImageRef get(const eacp::Graphics::Image& image);

    // Drops every texture nothing outside the cache is holding: one whose last
    // recording was cleared, or whose last holder has gone. The host calls this
    // after each recording walk.
    void releaseUnused();

    // Lets go of every texture, so the next ask for each image uploads it
    // again: what a replaced device needs, nothing made on the old one being
    // drawable on the new. A caller still holding a reference holds the old
    // texture, and has to ask again.
    void clear();

    int size() const;

private:
    // What a pixel buffer hashed to, as of a write count. The buffer is held
    // weakly: a strong reference would keep every image ever drawn alive, and
    // would count as a copy, so the image's own next write would detach for no
    // reason. Expired, it also says the address it is filed under may have been
    // reused by a buffer it never saw -- which cannot happen while the buffer is
    // allocated with its control block, the weak reference holding the block,
    // and is checked all the same rather than left to how Image allocates.
    struct Known
    {
        std::weak_ptr<const void> buffer;
        std::uint64_t generation = 0;
        std::uint64_t hash = 0;
    };

    std::uint64_t hashFor(const eacp::Graphics::Image& image);

    std::unordered_map<std::uint64_t, ImageRef> entries;
    std::unordered_map<const void*, Known> known;
};
} // namespace eacp::UI
