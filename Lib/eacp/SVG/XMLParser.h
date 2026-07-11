#pragma once

#include "SVGElement.h"

namespace eacp::SVG
{

std::optional<SVGElement> parseXML(std::string_view input);

} // namespace eacp::SVG
