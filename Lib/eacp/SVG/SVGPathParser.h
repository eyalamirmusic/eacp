#pragma once

#include "Common.h"

#include <eacp/GPUWidgets/Path/Path.h>

namespace eacp::SVG
{

// A `d` attribute as a path.
//
// Templated on the path type rather than fixed to one, because eacp has two and
// the body cannot tell them apart: Graphics::Path wraps a native CGPath or
// Direct2D geometry, GPUWidgets::Path holds the flattened points the coverage
// rasterizer reads, and every call this parser makes - moveTo, lineTo, quadTo,
// cubicTo, close - is on both with the same signature. Which is not a
// coincidence: the GPU one was written to mirror the native one.
//
// Instantiated for exactly those two, so a third is a link error naming the type
// rather than a body silently recompiled in every translation unit that
// included this.
//
// Appends rather than returns, because a GPUWidgets::Path flattens its curves as
// they are added and the tolerance it flattens to has to be set on the path
// first. A caller that hands one in can choose that; one that takes a fresh path
// back cannot, and would be stuck with whatever the default happened to be - the
// wrong tolerance for geometry about to be scaled up, and ten times too loose
// for anything about to be stroked.
template <typename PathType>
void parseSVGPathInto(const std::string& d, PathType& path);

extern template void parseSVGPathInto<Graphics::Path>(const std::string&,
                                                      Graphics::Path&);
extern template void parseSVGPathInto<GPUWidgets::Path>(const std::string&,
                                                        GPUWidgets::Path&);

template <typename PathType>
PathType parseSVGPath(const std::string& d)
{
    auto path = PathType();
    parseSVGPathInto(d, path);

    return path;
}

} // namespace eacp::SVG
