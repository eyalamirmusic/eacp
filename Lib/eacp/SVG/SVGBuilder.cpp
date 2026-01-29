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

static void buildRect(SVGView& view, const SVGElement& element)
{
    auto x = element.numAttr("x");
    auto y = element.numAttr("y");
    auto w = element.numAttr("width");
    auto h = element.numAttr("height");
    auto rx = element.numAttr("rx");
    auto ry = element.numAttr("ry");

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

static void buildCircle(SVGView& view, const SVGElement& element)
{
    auto cx = element.numAttr("cx");
    auto cy = element.numAttr("cy");
    auto r = element.numAttr("r");

    Graphics::Path path;
    path.addEllipse({cx - r, cy - r, r * 2.f, r * 2.f});
    addShapeLayer(view, element, std::move(path));
}

static void buildEllipse(SVGView& view, const SVGElement& element)
{
    auto cx = element.numAttr("cx");
    auto cy = element.numAttr("cy");
    auto rx = element.numAttr("rx");
    auto ry = element.numAttr("ry");

    Graphics::Path path;
    path.addEllipse({cx - rx, cy - ry, rx * 2.f, ry * 2.f});
    addShapeLayer(view, element, std::move(path));
}

static void buildLine(SVGView& view, const SVGElement& element)
{
    auto x1 = element.numAttr("x1");
    auto y1 = element.numAttr("y1");
    auto x2 = element.numAttr("x2");
    auto y2 = element.numAttr("y2");

    Graphics::Path path;
    path.moveTo({x1, y1});
    path.lineTo({x2, y2});
    addShapeLayer(view, element, std::move(path));
}

static void buildPolyline(SVGView& view, const SVGElement& element, bool close)
{
    auto pointsStr = element.attr("points");
    auto points = parsePointList(pointsStr);
    if (points.empty())
        return;

    Graphics::Path path;
    path.moveTo(points[0]);
    for (size_t i = 1; i < points.size(); ++i)
        path.lineTo(points[i]);
    if (close)
        path.close();

    addShapeLayer(view, element, std::move(path));
}

static void buildPath(SVGView& view, const SVGElement& element)
{
    auto d = element.attr("d");
    if (d.empty())
        return;

    auto path = parseSVGPath(d);
    addShapeLayer(view, element, std::move(path));
}

static void buildText(SVGView& view, const SVGElement& element)
{
    auto x = element.numAttr("x");
    auto y = element.numAttr("y");
    auto fontSize = element.numAttr("font-size", 16.f);
    auto fontFamily = element.attr("font-family", "Helvetica");
    auto text = element.textContent;

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

    layer->setBounds({drawX, drawY, textWidth, textHeight});

    auto opacity = element.attr("opacity");
    if (!opacity.empty())
        layer->setOpacity(std::stof(opacity));

    view.addLayer(*layer);
    view.ownedTextLayers.push_back(std::move(layer));
}

static void buildElement(SVGView& view, const SVGElement& element);

static void buildGroup(SVGView& parent, const SVGElement& element)
{
    auto child = std::make_unique<SVGView>();

    auto transform = element.attr("transform");
    if (!transform.empty())
    {
        auto t = parseTransform(transform);
        child->setBounds({t.translateX,
                          t.translateY,
                          parent.getBounds().w,
                          parent.getBounds().h});
    }

    for (auto& childElement: element.children)
        buildElement(*child, childElement);

    parent.addSubview(*child);
    parent.ownedChildren.push_back(std::move(child));
}

static void buildElement(SVGView& view, const SVGElement& element)
{
    auto& tag = element.tag;

    if (tag == "rect")
        buildRect(view, element);
    else if (tag == "circle")
        buildCircle(view, element);
    else if (tag == "ellipse")
        buildEllipse(view, element);
    else if (tag == "line")
        buildLine(view, element);
    else if (tag == "polyline")
        buildPolyline(view, element, false);
    else if (tag == "polygon")
        buildPolyline(view, element, true);
    else if (tag == "path")
        buildPath(view, element);
    else if (tag == "text")
        buildText(view, element);
    else if (tag == "g")
        buildGroup(view, element);
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
    result.root->setBounds({0, 0, width, height});

    for (auto& child: root.children)
        buildElement(*result.root, child);

    return result;
}

} // namespace eacp::SVG
