#pragma once

#include "Common.h"

namespace eacp::SVG
{

struct SVGElement
{
    std::string attr(const std::string& name,
                     const std::string& fallback = "") const;

    float numAttr(const std::string& name, float fallback = 0.f) const;

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
void collectIds(const SVGElement& element, ElementsById& byId);

} // namespace eacp::SVG
