#include "SVGParser.h"
#include "XMLParser.h"

namespace eacp::SVG
{

ParseResult parse(const std::string& svgMarkup)
{
    auto element = parseXML(svgMarkup);
    if (!element || element->tag != "svg")
        return {};

    return buildSVG(*element);
}

} // namespace eacp::SVG