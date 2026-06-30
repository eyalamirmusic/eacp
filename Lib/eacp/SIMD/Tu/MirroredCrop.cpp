#include <eacp/SIMD/SIMD.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// Crop a width x height region at (x, y) of a tightly-packed RGBA8 image and
// mirror it horizontally, in a single pass. This only moves bytes, so it is
// memory-bandwidth-bound -- there is no SIMD win (cf. swapRedBlue/copy). It lives
// in eacp-simd purely so it is always built -O3 (the gesture-camera fast path)
// without needing a per-source compile flag in the Graphics module. The caller
// guarantees the crop region lies within the source.
namespace eacp::simd
{

void mirroredCrop(const std::uint8_t* src,
                  int srcWidth,
                  int x,
                  int y,
                  int width,
                  int height,
                  std::uint8_t* dst)
{
    for (int dy = 0; dy < height; ++dy)
    {
        const std::uint8_t* srcRow =
            src + (static_cast<std::size_t>(y + dy) * srcWidth + x) * 4;
        std::uint8_t* dstRow = dst + static_cast<std::size_t>(dy) * width * 4;
        for (int dx = 0; dx < width; ++dx)
            std::memcpy(dstRow + dx * 4, srcRow + (width - 1 - dx) * 4, 4);
    }
}

} // namespace eacp::simd
