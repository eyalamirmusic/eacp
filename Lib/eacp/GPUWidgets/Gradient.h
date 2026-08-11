#pragma once

#include "Common.h"

namespace eacp::GPUWidgets
{
// Point is in the gradient's coordinate space; stops need not be pre-sorted.
Graphics::Color colorAt(const Graphics::LinearGradient& gradient,
                        const Graphics::Point& point);
} // namespace eacp::GPUWidgets
