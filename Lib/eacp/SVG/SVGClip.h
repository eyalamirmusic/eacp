#pragma once

#include "SVGAttributes.h"

#include <eacp/GPUWidgets/Path/PathRasterizer.h>

#include <optional>

namespace eacp::SVG
{
// The region a clip-path names: one path, in the coordinates of whatever
// referenced it.
//
// One path and not a list, because a <clipPath> holding several shapes clips to
// their *union* -- and a union of contours wound the same way is what the
// non-zero rule already computes, so the shapes are appended into one region
// rather than intersected afterwards by anything.
struct ClipRegion
{
    GPUWidgets::Path path;
    GPUWidgets::FillRule rule = GPUWidgets::FillRule::NonZero;

    // Whether the reference named a <clipPath> at all, which is what tells the
    // two empty regions apart -- and they mean opposite things. A reference to
    // nothing, or to something that is not a clipPath, is ignored and the
    // element draws unclipped; a clipPath holding no geometry is a region of
    // nothing, and an element referencing one draws not at all.
    bool resolved = false;

    bool isEmpty() const { return path.isEmpty(); }
};

// The region a `clip-path="url(#id)"` refers to, in the referencing element's
// own units -- so the caller maps it by the same transform it maps the element's
// geometry by, and the clip travels with what it clips.
//
// Empty when the id names nothing, names something that is not a <clipPath>, or
// names one with no geometry in it, and every one of those means the element is
// drawn unclipped rather than not drawn.
//
// Two things about a clipPath are worth saying, because they are what a document
// gets wrong quietly:
//
//  - clipPathUnits="objectBoundingBox" puts the region in fractions of the
//    referencing element's own bounding box, exactly as a gradient's default
//    units do, which is what `objectBounds` is for. The default is
//    userSpaceOnUse, where the numbers are the element's own coordinates and the
//    bounds are not read at all.
//  - clip-rule is the fill rule of the region, and it belongs to the children
//    rather than to the clipPath. A region built from more than one child is
//    non-zero whatever they said, that being the rule under which their union
//    is their union; a single child's own rule is honoured.
//
// `flatness` is the tolerance the curves are flattened to, in the same units the
// region comes out in, and `viewport` is what the percentages inside it are
// fractions of -- the one in force where the clip-path was written, the region
// belonging to the referencing element's space rather than to wherever in the
// tree the <clipPath> happens to sit.
ClipRegion resolveClipPath(const std::string& reference,
                           const ElementsById& byId,
                           const Graphics::Rect& objectBounds,
                           const Viewport& viewport,
                           float flatness);

// The rectangle a path is, when it is exactly one -- four corners, axis-aligned
// edges -- and nothing when it is anything else.
//
// Worth asking of a clip, because a rectangular one needs no mask at all: it is
// a scissor rect, which costs the atlas nothing, nests exactly with every other
// rectangle in force, and cuts the glyphs a mask cannot reach. A viewport clip
// is the common case and it is always this.
std::optional<Graphics::Rect> asAxisAlignedRect(const GPUWidgets::Path& path);

// Whether the region is placed against the geometry it cuts rather than in the
// document's own units -- clipPathUnits="objectBoundingBox".
//
// Which a caller has to know before it resolves anything, because it is what
// decides whether two elements naming one clipPath share a region: in user
// space they do, and a group's clip is one mask however many children it cuts;
// against a bounding box they cannot, each element's box being its own.
bool clipUsesBoundingBox(const std::string& reference, const ElementsById& byId);
} // namespace eacp::SVG
