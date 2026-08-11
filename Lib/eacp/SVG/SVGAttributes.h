#pragma once

#include "Common.h"
#include "SVGElement.h"

#include <eacp/GPUWidgets/Path/AffineTransform.h>
#include <eacp/UI/Render/Gradient.h>

namespace eacp::SVG
{

struct ColorResult
{
    Graphics::Color color;
    bool isNone = false;
};

ColorResult parseColor(const std::string& value);

// A transform list flattened into fields, which loses ordering, repeats, and
// matrix/skewX/skewY entirely. Only for SVGBuilder's child views, which can be
// moved but not transformed; anything building geometry wants the matrix below.
struct Transform
{
    float translateX = 0.f;
    float translateY = 0.f;
    float scaleX = 1.f;
    float scaleY = 1.f;
    float rotateDeg = 0.f;
};

Transform parseTransform(const std::string& value);

// Every function of the specification, composed in the order written. Angles
// are degrees, as the format has them.
GPUWidgets::AffineTransform parseTransformMatrix(const std::string& value);

// How a viewBox is fitted to its viewport. Said nothing, a document gets
// xMidYMid meet: uniform and centred, not stretch-to-fit.
struct PreserveAspectRatio
{
    enum class Align
    {
        Min,
        Mid,
        Max
    };

    // False only for preserveAspectRatio="none", where each axis fills its own
    // and the alignment has nothing to do.
    bool uniform = true;

    Align x = Align::Mid;
    Align y = Align::Mid;

    // meet fits the box inside the viewport; slice scales it to cover and lets
    // the overflow run past the edges, which the component's own clip cuts.
    bool slice = false;
};

PreserveAspectRatio parsePreserveAspectRatio(const std::string& value);

// The map from a viewBox onto the viewport it fills: what an <svg> does with
// its own box, and a <symbol> with the size its <use> gave it.
GPUWidgets::AffineTransform viewBoxTransform(const Graphics::Rect& viewBox,
                                             const Graphics::Rect& viewport,
                                             const PreserveAspectRatio& fit);

// A style attribute's declarations, by property name. A declaration block and
// nothing else: selectors are not read, so a <style> element is still ignored.
std::unordered_map<std::string, std::string>
    parseStyleDeclarations(const std::string& value);

// An element's presentation properties, a style declaration winning over the
// attribute of the same name as CSS requires. The declarations are parsed in
// the constructor, so reading many properties off one element is one parse.
struct PropertyReader
{
    explicit PropertyReader(const SVGElement& elementToUse)
        : element(elementToUse)
        , declarations(parseStyleDeclarations(elementToUse.attr("style")))
    {
    }

    std::string operator()(const std::string& name) const
    {
        auto found = declarations.find(name);

        return found != declarations.end() ? found->second : element.attr(name);
    }

    const SVGElement& element;
    std::unordered_map<std::string, std::string> declarations;
};

Vector<float> parseNumberList(const std::string& value);

Vector<Graphics::Point> parsePointList(const std::string& value);

// The id a paint value refers to, or empty when the value is a plain colour. A
// url() written with a fallback after it still names the reference; resolving
// it, and falling back, is the caller's business.
std::string parsePaintReference(const std::string& value);

// The id an element's href or xlink:href names -- SVG 2 dropped the namespace
// and older files still carry it -- or empty for another document.
std::string hrefId(const SVGElement& element);

} // namespace eacp::SVG
