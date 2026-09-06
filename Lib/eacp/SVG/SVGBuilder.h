#pragma once

#include "Common.h"

#include "SVGElement.h"

namespace eacp::SVG
{

// The native render tier: a view holding one retained layer per shape, which
// only a platform with its own 2D drawing stack has. The declarations here are
// portable; the definitions behind them are built under EACP_HAS_CONTEXT, so
// on Linux nothing in this file has a body. SVGComponent is the sibling that
// draws the same document through eacp-ui and builds everywhere.
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
