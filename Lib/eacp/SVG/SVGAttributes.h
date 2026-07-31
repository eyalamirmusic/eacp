#pragma once

#include "Common.h"

#include <eacp/GPUWidgets/Path/AffineTransform.h>

namespace eacp::SVG
{

struct ColorResult
{
    Graphics::Color color;
    bool isNone = false;
};

ColorResult parseColor(const std::string& value);

// A transform list flattened into fields, which loses what the list said.
// Ordering is gone - translate(..) rotate(..) and its reverse land here
// identically - a repeated function overwrites the one before it, and matrix,
// skewX and skewY have nowhere to go at all. Kept because SVGBuilder's native
// child views can only be moved, not transformed, so the translation is all it
// can use. Anything building geometry should read parseTransformMatrix.
struct Transform
{
    float translateX = 0.f;
    float translateY = 0.f;
    float scaleX = 1.f;
    float scaleY = 1.f;
    float rotateDeg = 0.f;
};

Transform parseTransform(const std::string& value);

// The same list as the matrix it actually denotes: every function of the
// specification, composed in the order written. Angles are degrees, as the
// format has them.
GPUWidgets::AffineTransform parseTransformMatrix(const std::string& value);

Vector<float> parseNumberList(const std::string& value);

Vector<Graphics::Point> parsePointList(const std::string& value);

} // namespace eacp::SVG
