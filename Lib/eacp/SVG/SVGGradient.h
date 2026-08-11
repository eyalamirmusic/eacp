#pragma once

#include "SVGAttributes.h"

namespace eacp::SVG
{
// Pad where the value says nothing, as the format has it.
UI::GradientSpread parseSpreadMethod(const std::string& value);

// The <stop> children in the order written; other children are skipped, a
// gradient being allowed to carry them.
Vector<UI::GradientStop> parseGradientStops(const SVGElement& gradient);

// The gradient a `fill="url(#id)"` names, in the space `transform` maps into,
// empty where it resolves to nothing. `objectBounds` is read only under the
// default gradientUnits="objectBoundingBox", `viewport` only under user space.
UI::Gradient resolveGradient(const std::string& id,
                             const ElementsById& byId,
                             const Graphics::Rect& objectBounds,
                             const Graphics::Rect& viewport,
                             const GPUWidgets::AffineTransform& transform);
} // namespace eacp::SVG
