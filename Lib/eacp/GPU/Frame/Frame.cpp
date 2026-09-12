#include "Frame.h"

namespace eacp::GPU
{
float Frame::backingScale() const
{
    return scale;
}

Graphics::Point Frame::logicalSize() const
{
    const auto pixels = pixelSize();
    const auto divisor = scale > 0.f ? scale : 1.f;

    return {pixels.x / divisor, pixels.y / divisor};
}
} // namespace eacp::GPU
