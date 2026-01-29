#pragma once

#include <eacp/Graphics/Graphics.h>
#include "SVGElement.h"
#include <memory>

namespace eacp::SVG
{

struct SVGView : Graphics::View
{
    std::vector<std::unique_ptr<SVGView>> ownedChildren;
    std::vector<std::unique_ptr<Graphics::ShapeLayer>> ownedLayers;
    std::vector<std::unique_ptr<Graphics::TextLayer>> ownedTextLayers;
};

struct ParseResult
{
    std::unique_ptr<SVGView> root;
    float width = 0.f;
    float height = 0.f;
};

ParseResult buildSVG(const SVGElement& root);

} // namespace eacp::SVG
