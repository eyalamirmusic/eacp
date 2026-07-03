#include "SVGShapes.h"
#include "SVGAttributes.h"
#include "SVGPathParser.h"

namespace eacp::SVG
{

namespace
{
Graphics::Path rectPath(const SVGElement& element, float sx, float sy)
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

    return path;
}

Graphics::Path ellipsePath(float cx, float cy, float rx, float ry)
{
    Graphics::Path path;
    path.addEllipse({cx - rx, cy - ry, rx * 2.f, ry * 2.f});
    return path;
}

Graphics::Path circlePath(const SVGElement& element, float sx, float sy)
{
    return ellipsePath(element.numAttr("cx") * sx,
                       element.numAttr("cy") * sy,
                       element.numAttr("r") * sx,
                       element.numAttr("r") * sy);
}

Graphics::Path ovalPath(const SVGElement& element, float sx, float sy)
{
    return ellipsePath(element.numAttr("cx") * sx,
                       element.numAttr("cy") * sy,
                       element.numAttr("rx") * sx,
                       element.numAttr("ry") * sy);
}

Graphics::Path linePath(const SVGElement& element, float sx, float sy)
{
    Graphics::Path path;
    path.moveTo({element.numAttr("x1") * sx, element.numAttr("y1") * sy});
    path.lineTo({element.numAttr("x2") * sx, element.numAttr("y2") * sy});
    return path;
}

std::optional<Graphics::Path>
    polylinePath(const SVGElement& element, bool close, float sx, float sy)
{
    auto points = parsePointList(element.attr("points"));
    if (points.empty())
        return std::nullopt;

    Graphics::Path path;
    path.moveTo({points[0].x * sx, points[0].y * sy});
    for (auto i = 1; i < points.size(); ++i)
        path.lineTo({points[i].x * sx, points[i].y * sy});
    if (close)
        path.close();

    return path;
}

std::optional<Graphics::Path>
    pathDataPath(const SVGElement& element, float sx, float sy)
{
    auto d = element.attr("d");
    if (d.empty())
        return std::nullopt;

    return parseSVGPath(d).scaled(sx, sy);
}
} // namespace

std::optional<Graphics::Path>
    makeShapePath(const SVGElement& element, float sx, float sy)
{
    auto& tag = element.tag;

    if (tag == "rect")
        return rectPath(element, sx, sy);
    if (tag == "circle")
        return circlePath(element, sx, sy);
    if (tag == "ellipse")
        return ovalPath(element, sx, sy);
    if (tag == "line")
        return linePath(element, sx, sy);
    if (tag == "polyline")
        return polylinePath(element, false, sx, sy);
    if (tag == "polygon")
        return polylinePath(element, true, sx, sy);
    if (tag == "path")
        return pathDataPath(element, sx, sy);

    return std::nullopt;
}

Graphics::Point documentSize(const SVGElement& root)
{
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

    return {width, height};
}

} // namespace eacp::SVG
