#pragma once

#include <eacp/Graphics/Graphics.h>
#include "SVGElement.h"
#include <eacp/Core/Utils/Containers.h>

namespace eacp::SVG
{

struct SVGView : Graphics::View
{
    void stretchToFit();
    void resized() override;
    void clearContent();

    SVGElement svgRoot;
    float svgWidth = 0.f;
    float svgHeight = 0.f;
    bool stretching = false;

    OwnedVector<SVGView> ownedChildren;
    OwnedVector<Graphics::ShapeLayer> ownedLayers;
    OwnedVector<Graphics::TextLayer> ownedTextLayers;
};

struct ParseResult
{
    OwningPointer<SVGView> root;
    float width = 0.f;
    float height = 0.f;
};

ParseResult buildSVG(const SVGElement& root);

} // namespace eacp::SVG
