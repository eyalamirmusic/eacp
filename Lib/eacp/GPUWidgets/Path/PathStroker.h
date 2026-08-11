#pragma once

#include "Path.h"

namespace eacp::GPUWidgets
{
// Only Butt keeps the stroke inside the path's own bounds.
enum class LineCap
{
    Butt,
    Round,
    Square
};

enum class LineJoin
{
    Miter,
    Round,
    Bevel
};

struct StrokeStyle
{
    float width = 1.f;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;

    // The longest miter, as a multiple of width; past it the corner bevels.
    float miterLimit = 4.f;
};

// Alternating on / off lengths, in path units, starting `offset` into the
// pattern.
struct DashPattern
{
    Vector<float> lengths;
    float offset = 0.f;

    // Also true of a list holding a negative entry, which invalidates all of it.
    bool isEmpty() const;
};

// Cuts the centre line, so it must run before strokeToFill. Closed sub-paths
// come back as open pieces, and lengths are measured along the flattened
// polyline rather than the curve.
Path dashPath(const Path& path, const DashPattern& dash);

// The region a stroke covers, as overlapping same-wound contours that must be
// filled **non-zero** - even-odd reads every overlap as a hole. Offsetting
// amplifies flattening error, so set a tighter Path::setFlatness before building.
Path strokeToFill(const Path& path, const StrokeStyle& style);
} // namespace eacp::GPUWidgets
