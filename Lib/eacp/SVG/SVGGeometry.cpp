#include "SVGGeometry.h"

#include "SVGAttributes.h"
#include "SVGPathParser.h"

#include <algorithm>

namespace eacp::SVG
{
namespace
{
void addRectGeometry(GPUWidgets::Path& path,
                     const SVGElement& element,
                     const Viewport& viewport)
{
    auto across = [&](const std::string& name)
    { return lengthAttr(element, name, viewport, LengthAxis::Horizontal); };

    auto down = [&](const std::string& name)
    { return lengthAttr(element, name, viewport, LengthAxis::Vertical); };

    auto rect =
        Graphics::Rect {across("x"), down("y"), across("width"), down("height")};

    if (rect.w <= 0.f || rect.h <= 0.f)
        return;

    auto rx = across("rx");
    auto ry = down("ry");

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

GPUWidgets::Path buildGeometry(const SVGElement& element,
                               const Viewport& viewport,
                               float flatness)
{
    auto path = GPUWidgets::Path {};
    path.setFlatness(flatness);

    auto across = [&](const std::string& name)
    { return lengthAttr(element, name, viewport, LengthAxis::Horizontal); };

    auto down = [&](const std::string& name)
    { return lengthAttr(element, name, viewport, LengthAxis::Vertical); };

    auto& tag = element.tag;

    if (tag == "rect")
        addRectGeometry(path, element, viewport);
    else if (tag == "circle")
    {
        // A circle's one radius belongs to neither axis, so it is the diagonal
        // over root two that a percentage of it is a percentage of.
        auto r = lengthAttr(element, "r", viewport, LengthAxis::Diagonal);

        addEllipseGeometry(path, across("cx"), down("cy"), r, r);
    }
    else if (tag == "ellipse")
        addEllipseGeometry(path, across("cx"), down("cy"), across("rx"), down("ry"));
    else if (tag == "line")
    {
        path.moveTo({across("x1"), down("y1")});
        path.lineTo({across("x2"), down("y2")});
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
