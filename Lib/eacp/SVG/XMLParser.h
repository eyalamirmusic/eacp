#pragma once

/** @file A minimal XML parser producing the SVG element tree. */

#include "SVGElement.h"

namespace eacp::SVG
{

// Parses markup into its root element; empty when the markup does not parse.
std::optional<SVGElement> parseXML(std::string_view input);

} // namespace eacp::SVG
