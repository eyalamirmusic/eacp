#pragma once

#include "Path.h"

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Graphics/Primitives/Primitives.h>

namespace eacp::GPUWidgets
{
// Triangulates a path's filled interior into a triangle list: a flat array where
// every three consecutive points form one triangle, ready to upload as a vertex
// buffer. Each sub-path is filled independently as a simple polygon via ear
// clipping; self-intersecting sub-paths and holes (even-odd / non-zero fills) are
// not handled in this version.
Vector<Graphics::Point> tessellateFill(const Path& path);
} // namespace eacp::GPUWidgets
