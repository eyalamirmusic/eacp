#pragma once

#include "Common.h"

#include <eacp/GPUWidgets/Path/Path.h>

namespace eacp::SVG
{

// A `d` attribute appended to a path, and appended rather than returned so the
// caller can set a GPUWidgets::Path's flatness first -- it flattens curves as
// they are added. Instantiated for Graphics::Path and GPUWidgets::Path only.
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
