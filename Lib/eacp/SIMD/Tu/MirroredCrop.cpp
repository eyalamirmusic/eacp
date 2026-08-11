#include "../SIMD.h"
#include "../Common.h"

// Byte movement only, so memory-bandwidth-bound with no SIMD win; it lives here
// purely to be built -O3. The caller guarantees the crop region lies within the
// source.
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
