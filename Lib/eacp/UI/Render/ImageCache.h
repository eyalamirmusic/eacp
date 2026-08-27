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
// and it is also what it costs -- see get().
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
    // Every pixel is hashed to find it, so a caller drawing a large image from
    // a paint() that runs often should keep what this returns rather than ask
    // each time: the reference is the handle, and holding it is what keeps the
    // texture alive across frames.
    ImageRef get(const eacp::Graphics::Image& image);

    // Drops every texture nothing outside the cache is holding: one whose last
    // recording was cleared, or whose last holder has gone. The host calls this
    // after each recording walk.
    void releaseUnused();

    int size() const { return (int) entries.size(); }

private:
    std::unordered_map<std::uint64_t, ImageRef> entries;
};
} // namespace eacp::UI
