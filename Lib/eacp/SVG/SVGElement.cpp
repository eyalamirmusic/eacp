#include "SVGElement.h"

#include <eacp/Core/Utils/Strings.h>

namespace eacp::SVG
{

std::string SVGElement::attr(const std::string& name,
                             const std::string& fallback) const
{
    auto it = attributes.find(name);
    if (it != attributes.end())
        return it->second;
    return fallback;
}

float SVGElement::numAttr(const std::string& name, float fallback) const
{
    auto it = attributes.find(name);
    if (it == attributes.end())
        return fallback;
    return Strings::parseFloatOr(it->second, fallback);
}

void collectIds(const SVGElement& element, ElementsById& byId)
{
    auto id = element.attr("id");

    if (!id.empty())
        byId.emplace(id, &element);

    for (const auto& child: element.children)
        collectIds(child, byId);
}

} // namespace eacp::SVG
