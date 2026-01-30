#include "SVGBuilder.h"
#include "SVGAttributes.h"
#include "SVGPathParser.h"

namespace eacp::SVG
{

static void applyFillAndStroke(Graphics::ShapeLayer& layer,
                               const SVGElement& element)
{
    auto fillStr = element.attr("fill", "black");
    auto fillResult = parseColor(fillStr);
    if (!fillResult.isNone)
        layer.setFillColor(fillResult.color);

    auto strokeStr = element.attr("stroke");
    if (!strokeStr.empty())
    {
        auto strokeResult = parseColor(strokeStr);
        if (!strokeResult.isNone)
            layer.setStrokeColor(strokeResult.color);
    }

    auto strokeWidth = element.attr("stroke-width");
    if (!strokeWidth.empty())
        layer.setStrokeWidth(std::stof(strokeWidth));

    auto opacity = element.attr("opacity");
    if (!opacity.empty())
        layer.setOpacity(std::stof(opacity));
}

static void addShapeLayer(SVGView& view,
                          const SVGElement& element,
                          const Graphics::Path& path)
{
    auto layer = std::make_unique<Graphics::ShapeLayer>();
    layer->setPath(path);
    applyFillAndStroke(*layer, element);
    view.addLayer(*layer);
    view.ownedLayers.push_back(std::move(layer));
}

static void buildRect(SVGView& view, const SVGElement& element, float sx, float sy)
{
    auto x = element.numAttr("x") * sx;
    auto y = element.numAttr("y") * sy;
    auto w = element.numAttr("width") * sx;
    auto h = element.numAttr("height") * sy;
    auto rx = element.numAttr("rx") * sx;
    auto ry = element.numAttr("ry") * sy;

    if (rx <= 0.f && ry > 0.f)
        rx = ry;
    if (ry <= 0.f && rx > 0.f)
        ry = rx;

    Graphics::Path path;
    Graphics::Rect rect {x, y, w, h};

    if (rx > 0.f || ry > 0.f)
        path.addRoundedRect(rect, rx);
    else
        path.addRect(rect);

    addShapeLayer(view, element, std::move(path));
}

static void buildCircle(SVGView& view, const SVGElement& element, float sx, float sy)
{
    auto cx = element.numAttr("cx") * sx;
    auto cy = element.numAttr("cy") * sy;
    auto rx = element.numAttr("r") * sx;
    auto ry = element.numAttr("r") * sy;

    Graphics::Path path;
    path.addEllipse({cx - rx, cy - ry, rx * 2.f, ry * 2.f});
    addShapeLayer(view, element, std::move(path));
}

static void
    buildEllipse(SVGView& view, const SVGElement& element, float sx, float sy)
{
    auto cx = element.numAttr("cx") * sx;
    auto cy = element.numAttr("cy") * sy;
    auto rx = element.numAttr("rx") * sx;
    auto ry = element.numAttr("ry") * sy;

    Graphics::Path path;
    path.addEllipse({cx - rx, cy - ry, rx * 2.f, ry * 2.f});
    addShapeLayer(view, element, std::move(path));
}

static void buildLine(SVGView& view, const SVGElement& element, float sx, float sy)
{
    auto x1 = element.numAttr("x1") * sx;
    auto y1 = element.numAttr("y1") * sy;
    auto x2 = element.numAttr("x2") * sx;
    auto y2 = element.numAttr("y2") * sy;

    Graphics::Path path;
    path.moveTo({x1, y1});
    path.lineTo({x2, y2});
    addShapeLayer(view, element, std::move(path));
}

static void buildPolyline(
    SVGView& view, const SVGElement& element, bool close, float sx, float sy)
{
    auto pointsStr = element.attr("points");
    auto points = parsePointList(pointsStr);
    if (points.empty())
        return;

    Graphics::Path path;
    path.moveTo({points[0].x * sx, points[0].y * sy});
    for (size_t i = 1; i < points.size(); ++i)
        path.lineTo({points[i].x * sx, points[i].y * sy});
    if (close)
        path.close();

    addShapeLayer(view, element, std::move(path));
}

static void buildPath(SVGView& view, const SVGElement& element, float sx, float sy)
{
    auto d = element.attr("d");
    if (d.empty())
        return;

    auto path = parseSVGPath(d).scaled(sx, sy);
    addShapeLayer(view, element, std::move(path));
}

static void buildText(SVGView& view, const SVGElement& element, float sx, float sy)
{
    auto x = element.numAttr("x") * sx;
    auto y = element.numAttr("y") * sy;
    auto baseFontSize = element.numAttr("font-size", 16.f);
    auto fontFamily = element.attr("font-family", "Helvetica");
    auto text = element.textContent;

    auto fontSizeX = baseFontSize * sx;
    auto fontSizeY = baseFontSize * sy;
    auto fontSize = std::min(fontSizeX, fontSizeY);

    auto layer = std::make_unique<Graphics::TextLayer>();
    layer->setText(text);

    auto fontOptions =
        Graphics::FontOptions().withName(fontFamily).withSize(fontSize);
    layer->setFont(Graphics::Font(fontOptions));

    auto fillStr = element.attr("fill", "black");
    auto fillResult = parseColor(fillStr);
    if (!fillResult.isNone)
        layer->setColor(fillResult.color);

    auto textWidth = fontSize * static_cast<float>(text.size()) * 0.6f;
    auto textHeight = fontSize * 1.2f;

    auto anchor = element.attr("text-anchor", "start");
    auto drawX = x;
    if (anchor == "middle")
        drawX = x - textWidth / 2.f;
    else if (anchor == "end")
        drawX = x - textWidth;

    auto drawY = y - fontSize;

    layer->setPosition({drawX, drawY});
    layer->setBounds({0, 0, textWidth, textHeight});

    auto opacity = element.attr("opacity");
    if (!opacity.empty())
        layer->setOpacity(std::stof(opacity));

    view.addLayer(*layer);
    view.ownedTextLayers.push_back(std::move(layer));
}

static void
    buildElement(SVGView& view, const SVGElement& element, float sx, float sy);

static void
    buildGroup(SVGView& parent, const SVGElement& element, float sx, float sy)
{
    auto child = std::make_unique<SVGView>();

    auto transform = element.attr("transform");
    if (!transform.empty())
    {
        auto t = parseTransform(transform);
        child->setBounds({t.translateX * sx,
                          t.translateY * sy,
                          parent.getBounds().w,
                          parent.getBounds().h});
    }

    for (auto& childElement: element.children)
        buildElement(*child, childElement, sx, sy);

    parent.addSubview(*child);
    parent.ownedChildren.push_back(std::move(child));
}

static void
    buildElement(SVGView& view, const SVGElement& element, float sx, float sy)
{
    auto& tag = element.tag;

    if (tag == "rect")
        buildRect(view, element, sx, sy);
    else if (tag == "circle")
        buildCircle(view, element, sx, sy);
    else if (tag == "ellipse")
        buildEllipse(view, element, sx, sy);
    else if (tag == "line")
        buildLine(view, element, sx, sy);
    else if (tag == "polyline")
        buildPolyline(view, element, false, sx, sy);
    else if (tag == "polygon")
        buildPolyline(view, element, true, sx, sy);
    else if (tag == "path")
        buildPath(view, element, sx, sy);
    else if (tag == "text")
        buildText(view, element, sx, sy);
    else if (tag == "g")
        buildGroup(view, element, sx, sy);
}

static void buildContent(SVGView& view, const SVGElement& root, float sx, float sy)
{
    for (auto& child: root.children)
        buildElement(view, child, sx, sy);
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
    result.root = std::make_unique<SVGView>();

    auto width = root.numAttr("width", 300.f);
    auto height = root.numAttr("height", 150.f);

    auto viewBox = root.attr("viewBox");
    if (!viewBox.empty())
    {
        auto nums = parseNumberList(viewBox);
        if (nums.size() >= 4)
        {
            if (width <= 0.f)
                width = nums[2];
            if (height <= 0.f)
                height = nums[3];
        }
    }

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
