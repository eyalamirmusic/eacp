#pragma once

#include "Common.h"

namespace eacp::SVG
{

struct SVGElement
{
    std::string attr(const std::string& name, const std::string& fallback = "") const
    {
        auto it = attributes.find(name);
        if (it != attributes.end())
            return it->second;
        return fallback;
    }

    float numAttr(const std::string& name, float fallback = 0.f) const
    {
        auto it = attributes.find(name);
        if (it == attributes.end())
            return fallback;
        return Strings::parseFloatOr(it->second, fallback);
    }

    std::string tag;
    std::unordered_map<std::string, std::string> attributes;
    Vector<SVGElement> children;
    std::string textContent;
};

// Every element in a document that named itself, which is what a reference has
// to be looked up in: a <use>, a gradient's href chain, a clip-path.
using ElementsById = std::unordered_map<std::string, const SVGElement*>;

// First wins where a document repeats an id, which is what a browser does with
// the same mistake.
inline void collectIds(const SVGElement& element, ElementsById& byId)
{
    auto id = element.attr("id");

    if (!id.empty())
        byId.emplace(id, &element);

    for (const auto& child: element.children)
        collectIds(child, byId);
}

} // namespace eacp::SVG
