#pragma once

#include "SVGElement.h"
#include <string_view>
#include <optional>

namespace eacp::SVG
{

std::optional<SVGElement> parseXML(std::string_view input);

} // namespace eacp::SVG
