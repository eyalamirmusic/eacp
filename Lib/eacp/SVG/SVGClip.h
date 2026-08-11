#pragma once

#include "SVGAttributes.h"

#include <eacp/GPUWidgets/Path/PathRasterizer.h>

#include <optional>

namespace eacp::SVG
{
// The region a clip-path names, in the coordinates of whatever referenced it.
// One path and not a list: a <clipPath> of several shapes clips to their union,
// which is what the non-zero rule computes over the appended contours.
struct ClipRegion
{
    GPUWidgets::Path path;
    GPUWidgets::FillRule rule = GPUWidgets::FillRule::NonZero;

    // Which tells the two empty regions apart: unresolved draws the element
    // unclipped, while a resolved clipPath holding no geometry draws nothing.
    bool resolved = false;

    bool isEmpty() const { return path.isEmpty(); }
};

// The region a `clip-path="url(#id)"` names, in the referencing element's own
// units. `objectBounds` is read only under clipPathUnits="objectBoundingBox";
// `flatness` is the curve tolerance, in those same units.
ClipRegion resolveClipPath(const std::string& reference,
                           const ElementsById& byId,
                           const Graphics::Rect& objectBounds,
                           float flatness);

// The rectangle a path is, when it is exactly one, and nothing otherwise. Worth
// asking of a clip: a rectangular one is a scissor rect, needing no mask.
std::optional<Graphics::Rect> asAxisAlignedRect(const GPUWidgets::Path& path);

// clipPathUnits="objectBoundingBox", which a caller has to know before it
// resolves anything: only elements in user space can share one region.
bool clipUsesBoundingBox(const std::string& reference, const ElementsById& byId);
} // namespace eacp::SVG
