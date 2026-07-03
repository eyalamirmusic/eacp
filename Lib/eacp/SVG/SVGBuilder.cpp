#include "SVGBuilder.h"
#include "SVGAttributes.h"
#include "SVGShapes.h"

#include <eacp/Core/Utils/Strings.h>

namespace eacp::SVG
{

static void applyFillAndStroke(Graphics::ShapeLayer& layer,
                               const SVGElement& element,
                               const InheritedStyle& style,
                               float sx,
                               float sy)
{
    auto fillResult = parseColor(style.fill);
    if (!fillResult.isNone)
        layer.setFillColor(fillResult.color);

    if (!style.stroke.empty())
    {
        auto strokeResult = parseColor(style.stroke);
        if (!strokeResult.isNone)
            layer.setStrokeColor(strokeResult.color);
    }

    // Geometry is scaled by (sx, sy), so stroke widths must scale with it;
    // a layer has a single stroke width, so non-uniform stretch averages out.
    if (!style.strokeWidth.empty())
        layer.setStrokeWidth(Strings::parseFloatOr(style.strokeWidth) * (sx + sy)
                             * 0.5f);

    auto opacity = element.attr("opacity");
    if (!opacity.empty())
        layer.setOpacity(Strings::parseFloatOr(opacity));
}

static void addShapeLayer(SVGView& view,
                          const SVGElement& element,
                          const InheritedStyle& style,
                          const Graphics::Path& path,
                          float sx,
                          float sy)
{
    auto& layer = view.ownedLayers.createNew();
    layer.setPath(path);
    applyFillAndStroke(layer, element, style, sx, sy);
    view.addLayer(layer);
}

static void buildText(SVGView& view,
                      const SVGElement& element,
                      const InheritedStyle& style,
                      float sx,
                      float sy)
{
    auto x = element.numAttr("x") * sx;
    auto y = element.numAttr("y") * sy;
    auto baseFontSize = element.numAttr("font-size", 16.f);
    auto fontFamily = element.attr("font-family", "Helvetica");
    auto text = element.textContent;

    auto fontSizeX = baseFontSize * sx;
    auto fontSizeY = baseFontSize * sy;
    auto fontSize = std::min(fontSizeX, fontSizeY);

    auto& layer = view.ownedTextLayers.createNew();
    layer.setText(text);

    auto fontOptions =
        Graphics::FontOptions().withName(fontFamily).withSize(fontSize);
    layer.setFont(Graphics::Font(fontOptions));

    auto fillResult = parseColor(style.fill);
    if (!fillResult.isNone)
        layer.setColor(fillResult.color);

    auto textWidth = fontSize * static_cast<float>(text.size()) * 0.6f;
    auto textHeight = fontSize * 1.2f;

    auto anchor = element.attr("text-anchor", "start");
    auto drawX = x;
    if (anchor == "middle")
        drawX = x - textWidth / 2.f;
    else if (anchor == "end")
        drawX = x - textWidth;

    auto drawY = y - fontSize;

    layer.setPosition({drawX, drawY});
    layer.setBounds({0, 0, textWidth, textHeight});

    auto opacity = element.attr("opacity");
    if (!opacity.empty())
        layer.setOpacity(Strings::parseFloatOr(opacity));

    view.addLayer(layer);
}

static void buildElement(SVGView& view,
                         const SVGElement& element,
                         const InheritedStyle& style,
                         float sx,
                         float sy);

static void buildGroup(SVGView& parent,
                       const SVGElement& element,
                       const InheritedStyle& style,
                       float sx,
                       float sy)
{
    auto& child = parent.ownedChildren.createNew();

    auto transform = element.attr("transform");
    if (!transform.empty())
    {
        auto t = parseTransform(transform);
        child.setBounds({t.translateX * sx,
                         t.translateY * sy,
                         parent.getBounds().w,
                         parent.getBounds().h});
    }

    for (auto& childElement: element.children)
        buildElement(child, childElement, style, sx, sy);

    parent.addSubview(child);
}

static void buildElement(SVGView& view,
                         const SVGElement& element,
                         const InheritedStyle& style,
                         float sx,
                         float sy)
{
    auto resolved = style.applied(element);

    if (auto path = makeShapePath(element, sx, sy))
        addShapeLayer(view, element, resolved, *path, sx, sy);
    else if (element.tag == "text")
        buildText(view, element, resolved, sx, sy);
    else if (element.tag == "g")
        buildGroup(view, element, resolved, sx, sy);
}

static void buildContent(SVGView& view, const SVGElement& root, float sx, float sy)
{
    auto style = InheritedStyle {}.applied(root);
    for (auto& child: root.children)
        buildElement(view, child, style, sx, sy);
}

void SVGView::clearContent()
{
    for (auto& layer: ownedLayers)
        removeLayer(*layer);
    for (auto& layer: ownedTextLayers)
        removeLayer(*layer);

    ownedChildren.clear();
    ownedLayers.clear();
    ownedTextLayers.clear();
}

void SVGView::stretchToFit()
{
    stretching = true;
    resized();
}

void SVGView::resized()
{
    if (!stretching || svgWidth <= 0.f || svgHeight <= 0.f)
        return;

    auto bounds = getLocalBounds();
    if (bounds.w <= 0.f || bounds.h <= 0.f)
        return;

    auto sx = bounds.w / svgWidth;
    auto sy = bounds.h / svgHeight;

    clearContent();
    buildContent(*this, svgRoot, sx, sy);
}

ParseResult buildSVG(const SVGElement& root)
{
    ParseResult result;
    result.root.create();

    auto [width, height] = documentSize(root);

    result.width = width;
    result.height = height;
    result.root->svgWidth = width;
    result.root->svgHeight = height;
    result.root->svgRoot = root;
    result.root->setBounds({0, 0, width, height});

    buildContent(*result.root, root, 1.f, 1.f);

    return result;
}

} // namespace eacp::SVG
