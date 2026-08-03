#include "SVGGeometry.h"

#include "SVGAttributes.h"
#include "SVGPathParser.h"

#include <algorithm>

namespace eacp::SVG
{
namespace
{
void addRectGeometry(GPUWidgets::Path& path, const SVGElement& element)
{
    auto rect = Graphics::Rect {element.numAttr("x"),
                                element.numAttr("y"),
                                element.numAttr("width"),
                                element.numAttr("height")};

    if (rect.w <= 0.f || rect.h <= 0.f)
        return;

    auto rx = element.numAttr("rx");
    auto ry = element.numAttr("ry");

    // Either radius alone stands for both, which is what the format says.
    if (rx <= 0.f)
        rx = ry;
    if (ry <= 0.f)
        ry = rx;

    // Path rounds corners with one radius rather than two, so an elliptical
    // corner becomes the circular one that fits inside it.
    if (rx > 0.f && ry > 0.f)
        path.addRoundedRect(rect, std::min(rx, ry));
    else
        path.addRect(rect);
}

void addEllipseGeometry(
    GPUWidgets::Path& path, float cx, float cy, float rx, float ry)
{
    if (rx <= 0.f || ry <= 0.f)
        return;

    path.addEllipse({cx - rx, cy - ry, rx * 2.f, ry * 2.f});
}

void addPolylineGeometry(GPUWidgets::Path& path,
                         const SVGElement& element,
                         bool closed)
{
    auto points = parsePointList(element.attr("points"));

    if (points.empty())
        return;

    path.moveTo(points[0]);

    for (auto i = 1; i < points.size(); ++i)
        path.lineTo(points[i]);

    if (closed)
        path.close();
}
} // namespace

bool isShapeTag(const std::string& tag)
{
    return tag == "rect" || tag == "circle" || tag == "ellipse" || tag == "line"
           || tag == "polyline" || tag == "polygon" || tag == "path";
}

GPUWidgets::Path buildGeometry(const SVGElement& element, float flatness)
{
    auto path = GPUWidgets::Path {};
    path.setFlatness(flatness);

    auto& tag = element.tag;

    if (tag == "rect")
        addRectGeometry(path, element);
    else if (tag == "circle")
        addEllipseGeometry(path,
                           element.numAttr("cx"),
                           element.numAttr("cy"),
                           element.numAttr("r"),
                           element.numAttr("r"));
    else if (tag == "ellipse")
        addEllipseGeometry(path,
                           element.numAttr("cx"),
                           element.numAttr("cy"),
                           element.numAttr("rx"),
                           element.numAttr("ry"));
    else if (tag == "line")
    {
        path.moveTo({element.numAttr("x1"), element.numAttr("y1")});
        path.lineTo({element.numAttr("x2"), element.numAttr("y2")});
    }
    else if (tag == "polyline")
        addPolylineGeometry(path, element, false);
    else if (tag == "polygon")
        addPolylineGeometry(path, element, true);
    else if (tag == "path")
        parseSVGPathInto(element.attr("d"), path);

    return path;
}
} // namespace eacp::SVG
