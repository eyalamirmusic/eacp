#pragma once

#include <eacp/Graphics/Graphics.h>
#include <string>

namespace eacp::SVG
{

Graphics::Path parseSVGPath(const std::string& d);

} // namespace eacp::SVG
