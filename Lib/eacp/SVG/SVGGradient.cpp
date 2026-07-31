#include "SVGGradient.h"

#include <cmath>

namespace eacp::SVG
{
namespace
{
// An href chain may be a cycle, which a document should not write and nothing
// stops it writing. Eight levels is past anything an honest file nests.
constexpr int maxReferenceDepth = 8;

const SVGElement* findById(const ElementsById& byId, const std::string& id)
{
    if (id.empty())
        return nullptr;

    auto found = byId.find(id);

    return found != byId.end() ? found->second : nullptr;
}

// The id an element's href names, or empty when it has none or names another
// document -- which there is nothing here to look in.
//
// Both spellings, because SVG 2 dropped the namespace and every file written
// before it still carries xlink:href.
std::string hrefId(const SVGElement& element)
{
    auto href = element.attr("href");

    if (href.empty())
        href = element.attr("xlink:href");

    return href.size() > 1 && href.front() == '#' ? href.substr(1) : std::string {};
}

// An attribute, following href to whatever the gradient inherits it from.
//
// Documents from every drawing program do this: one gradient carries the stops
// and a handful of others carry only a position and point at it. An attribute is
// answered by the first element in the chain that has it, which is what the
// specification says and the only reading under which those files come out
// right.
std::string inheritedAttribute(const SVGElement& gradient,
                               const ElementsById& byId,
                               const std::string& name)
{
    auto* element = &gradient;

    for (auto step = 0; step < maxReferenceDepth && element != nullptr; ++step)
    {
        auto value = element->attr(name);

        if (!value.empty())
            return value;

        element = findById(byId, hrefId(*element));
    }

    return {};
}

Vector<UI::GradientStop> inheritedStops(const SVGElement& gradient,
                                        const ElementsById& byId)
{
    auto* element = &gradient;

    for (auto step = 0; step < maxReferenceDepth && element != nullptr; ++step)
    {
        auto stops = parseGradientStops(*element);

        if (!stops.empty())
            return stops;

        element = findById(byId, hrefId(*element));
    }

    return {};
}
} // namespace

UI::GradientSpread parseSpreadMethod(const std::string& value)
{
    auto lowered = Strings::toLower(value);

    if (lowered == "reflect")
        return UI::GradientSpread::Reflect;

    if (lowered == "repeat")
        return UI::GradientSpread::Repeat;

    return UI::GradientSpread::Pad;
}

Vector<UI::GradientStop> parseGradientStops(const SVGElement& gradient)
{
    auto stops = Vector<UI::GradientStop> {};

    for (const auto& child: gradient.children)
    {
        if (child.tag != "stop")
            continue;

        auto declarations = parseStyleDeclarations(child.attr("style"));

        auto read = [&](const std::string& name)
        {
            auto found = declarations.find(name);

            return found != declarations.end() ? found->second : child.attr(name);
        };

        auto stop = UI::GradientStop {};

        // A percentage where a fraction is meant is common enough that it is not
        // worth telling the two apart anywhere but here.
        auto offset = child.attr("offset", "0");

        stop.position = Strings::parseFloatOr(offset, 0.f);

        if (!offset.empty() && offset.back() == '%')
            stop.position *= 0.01f;

        auto colour = parseColor(read("stop-color"));

        // A stop that says nothing is black, which is the format's answer and
        // not what an empty parse gives.
        stop.color = colour.isNone ? Graphics::Color::black() : colour.color;

        auto opacity = read("stop-opacity");

        if (!opacity.empty())
            stop.color = stop.color.withAlpha(stop.color.a
                                              * Strings::parseFloatOr(opacity, 1.f));

        stops.add(stop);
    }

    return stops;
}

UI::Gradient resolveGradient(const std::string& id,
                             const ElementsById& byId,
                             const Graphics::Rect& objectBounds,
                             const Graphics::Rect& viewport,
                             const GPUWidgets::AffineTransform& transform)
{
    auto* element = findById(byId, id);

    if (element == nullptr)
        return {};

    auto radial = element->tag == "radialGradient";

    if (!radial && element->tag != "linearGradient")
        return {};

    auto gradient = UI::Gradient {};
    gradient.kind = radial ? UI::Gradient::Kind::Radial : UI::Gradient::Kind::Linear;
    gradient.stops = inheritedStops(*element, byId);

    if (gradient.stops.empty())
        return {};

    gradient.spread =
        parseSpreadMethod(inheritedAttribute(*element, byId, "spreadMethod"));

    auto boundingBoxUnits =
        inheritedAttribute(*element, byId, "gradientUnits") != "userSpaceOnUse";

    // What a percentage is a percentage *of*. In bounding-box units the
    // coordinates are already fractions, so it is one; in user space it is the
    // viewport's own extent, and for a radius the diagonal over root two, which
    // is the length the specification names for a value with no axis of its own.
    auto acrossX = boundingBoxUnits ? 1.f : viewport.w;
    auto acrossY = boundingBoxUnits ? 1.f : viewport.h;
    auto diagonal =
        boundingBoxUnits
            ? 1.f
            : std::sqrt((viewport.w * viewport.w + viewport.h * viewport.h) * 0.5f);

    // Every one of these defaults is written as a percentage in the
    // specification -- 0%, 100%, 50% -- which is why the fallback is a fraction
    // of the same extent rather than a number: in user space an `x2` left out
    // means the viewport's right edge, not one unit across.
    auto number = [&](const std::string& name, float defaultFraction, float across)
    {
        auto value = inheritedAttribute(*element, byId, name);

        if (value.empty())
            return defaultFraction * across;

        auto number = Strings::parseFloatOr(value, defaultFraction * across);

        return value.back() == '%' ? number * 0.01f * across : number;
    };

    if (radial)
    {
        gradient.start = {number("cx", 0.5f, acrossX), number("cy", 0.5f, acrossY)};
        gradient.radius = number("r", 0.5f, diagonal);
    }
    else
    {
        gradient.start = {number("x1", 0.f, acrossX), number("y1", 0.f, acrossY)};
        gradient.end = {number("x2", 1.f, acrossX), number("y2", 0.f, acrossY)};
    }

    auto gradientTransform = inheritedAttribute(*element, byId, "gradientTransform");

    if (!gradientTransform.empty())
        gradient.transform = parseTransformMatrix(gradientTransform);

    // The bounding box goes on after gradientTransform, because with these units
    // the box *is* the space that transform was written in.
    if (boundingBoxUnits)
        gradient.transform =
            gradient.transform.then(GPUWidgets::AffineTransform {objectBounds.w,
                                                                 0.f,
                                                                 0.f,
                                                                 objectBounds.h,
                                                                 objectBounds.x,
                                                                 objectBounds.y});

    gradient.transform = gradient.transform.then(transform);

    return gradient;
}
} // namespace eacp::SVG
