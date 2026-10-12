#include "Copy.h"

namespace eacp::GPU
{
TextureCopyRegion TextureCopyRegion::wholeLevel(int level, int slice)
{
    auto region = TextureCopyRegion {};
    region.sourceLevel = level;
    region.sourceSlice = slice;
    region.destinationLevel = level;
    region.destinationSlice = slice;
    region.size.depth = 0;
    return region;
}
} // namespace eacp::GPU
