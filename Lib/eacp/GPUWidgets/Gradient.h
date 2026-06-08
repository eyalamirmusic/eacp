#pragma once

#include <eacp/Graphics/Primitives/Primitives.h>

namespace eacp::GPUWidgets
{
// The colour of a linear gradient at a point: projects the point onto the gradient
// axis (start -> end), clamps outside [start, end], then interpolates between the
// bracketing colour stops. Stops need not be pre-sorted. PathView calls this once
// per fill vertex to bake a gradient into a vertex-colour mesh.
Graphics::Color colorAt(const Graphics::LinearGradient& gradient,
                        const Graphics::Point& point);
} // namespace eacp::GPUWidgets
