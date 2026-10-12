#pragma once

#include <cstdint>

namespace eacp::GPU
{
// A texel position inside one level of one slice. z is the image of a volume
// (TextureType::ThreeD), and 0 on every other kind.
struct TextureOrigin
{
    int x = 0;
    int y = 0;
    int z = 0;
};

// How many texels a copy covers. A width, height or depth of 0 means "to the
// edge of the source level from the origin", so a default size copies the rest
// of the level. depth counts a volume's images and is 1 on every other kind.
struct TextureExtent
{
    int width = 0;
    int height = 0;
    int depth = 1;
};

// Part of one texture copied into another: a level and a slice on each side
// (a cube's face is its slice, 0 to 5 in +X, -X, +Y, -Y, +Z, -Z order; an
// array's layer is one too, and a cube array's layer * 6 + face), an origin on
// each side, and one size. A volume has the one slice and copies a box: the
// origins' z and the size's depth pick its images, a depth of 0 running to the
// edge of the source level. sliceCount copies that many consecutive slices
// from each side's slice at once, the same box of each; it is 1 on a volume.
// A volume copies to and from volumes only.
struct TextureCopyRegion
{
    int sourceLevel = 0;
    int sourceSlice = 0;
    TextureOrigin sourceOrigin;

    int destinationLevel = 0;
    int destinationSlice = 0;
    TextureOrigin destinationOrigin;

    TextureExtent size;

    int sliceCount = 1;

    // The whole of one level, the same level and slice on both sides: every
    // image of it on a volume.
    static TextureCopyRegion wholeLevel(int level, int slice = 0);
};

// The texture half of a copy between a buffer and a texture, and how the
// buffer lays the texels out: rows of bytesPerRow (0 for tightly packed)
// starting bufferOffset bytes in, a volume's images bytesPerImage apart (0 for
// bytesPerRow times the rows). The offset is a multiple of the texel size and
// of four, and bytesPerRow a multiple of the texel size.
struct BufferTextureCopy
{
    std::int64_t bufferOffset = 0;
    int bytesPerRow = 0;

    int level = 0;
    int slice = 0;
    TextureOrigin origin;
    TextureExtent size;

    int bytesPerImage = 0;
};
} // namespace eacp::GPU
