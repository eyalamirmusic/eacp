#pragma once

#include "SVGAttributes.h"

namespace eacp::SVG
{
// A gradient's spreadMethod, which is what happens past its two ends. The
// default is pad, and it is what a document that says nothing means.
UI::GradientSpread parseSpreadMethod(const std::string& value);

// The <stop> children of a gradient element, in the order written: offset,
// stop-color and stop-opacity, each of which may be a presentation attribute or
// a style declaration. Elements that are not stops are skipped rather than
// refused, a gradient being allowed to carry other children.
Vector<UI::GradientStop> parseGradientStops(const SVGElement& gradient);

// The gradient a `fill="url(#id)"` names, placed in the space `transform` maps
// into. Empty when the id names nothing, names something that is not a gradient,
// or names one with no stops anywhere in its href chain -- in each of which the
// shape falls back to the colour beside the reference, which is what the format
// says.
//
// Two coordinate systems have to be got right here and both are quiet when they
// are not:
//
//  - gradientUnits="objectBoundingBox", the default and what most documents use
//    without saying so, puts the gradient in fractions of the shape's own
//    bounding box -- so one definition means something different for every
//    element it paints, and a box that is not square is a non-uniform scale.
//    userSpaceOnUse puts it in the document's units instead, where `viewport`
//    is what a percentage is a percentage of.
//  - gradientTransform applies inside that space, before the box maps it out;
//    `transform` is the element's own, the one its geometry was baked through.
//
// The composition is left as a matrix on the result rather than resolved into
// endpoints, because a linear gradient under a non-uniform scale has bands that
// no pair of endpoints describes.
//
// `objectBounds` is the geometry's own bounding box in the document's units.
UI::Gradient resolveGradient(const std::string& id,
                             const ElementsById& byId,
                             const Graphics::Rect& objectBounds,
                             const Graphics::Rect& viewport,
                             const GPUWidgets::AffineTransform& transform);
} // namespace eacp::SVG
