#pragma once

#include <eacp/Graphics/Graphics.h>
#include "SVGElement.h"
#include <ea_data_structures/Pointers/OwningPointer.h>
#include <ea_data_structures/Structures/OwnedVector.h>

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

    EA::OwnedVector<SVGView> ownedChildren;
    EA::OwnedVector<Graphics::ShapeLayer> ownedLayers;
    EA::OwnedVector<Graphics::TextLayer> ownedTextLayers;
};

struct ParseResult
{
    EA::OwningPointer<SVGView> root;
    float width = 0.f;
    float height = 0.f;
};

ParseResult buildSVG(const SVGElement& root);

} // namespace eacp::SVG
