#include <eacp/SVG/SVG.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

// A gradient's colours are the easy half of it. The hard half is where it is
// put, and there are two coordinate systems to get wrong.
//
// The default units are fractions of the shape's own bounding box, so one
// definition means something different for every element it paints -- and
// gradientTransform applies *inside* that space rather than after it, which is
// the ordering nothing about a rendered picture tells you is wrong. Both
// mistakes draw a gradient of roughly the right colours in roughly the right
// place, which is why they are pinned here as arithmetic instead.
//
// The placement is left as a matrix rather than resolved into two endpoints, and
// that is not tidiness: a bounding box that is not square is a non-uniform
// scale, and a linear gradient under one has bands that are no longer
// perpendicular to the line between its ends. Only the shader can see that, but
// the matrix carrying it has to survive to reach one.

using namespace nano;
using namespace eacp;

namespace
{
constexpr auto tolerance = 0.001f;

bool isNear(float a, float b)
{
    return std::abs(a - b) < tolerance;
}

bool isNear(const Graphics::Point& point, float x, float y)
{
    return isNear(point.x, x) && isNear(point.y, y);
}

SVG::SVGElement documentFrom(const std::string& markup)
{
    auto root = SVG::parseXML(markup);

    return root.has_value() ? *root : SVG::SVGElement {};
}

// Where the gradient's own two ends land once everything has been composed,
// which is the whole of what the placement has to get right.
Graphics::Point endOf(const UI::Gradient& gradient, bool second)
{
    return gradient.transform.apply(second ? gradient.end : gradient.start);
}

const auto anyViewport = Graphics::Rect {0.f, 0.f, 200.f, 200.f};
const auto anyBox = Graphics::Rect {0.f, 0.f, 10.f, 10.f};
} // namespace

auto tPaintReference = test("SVGAttributes/aPaintNamesAGradientOrIsAColour") = []
{
    check(SVG::parsePaintReference("url(#fade)") == "fade");
    check(SVG::parsePaintReference("url('#fade')") == "fade");
    check(SVG::parsePaintReference("url(\"#fade\")") == "fade");

    // A fallback colour written after the reference is still a reference.
    check(SVG::parsePaintReference("url(#fade) red") == "fade");

    check(SVG::parsePaintReference("#ff0000").empty());
    check(SVG::parsePaintReference("none").empty());
    check(SVG::parsePaintReference("").empty());
};

auto tStops = test("SVGGradient/stopsReadOffsetsColoursAndOpacity") = []
{
    auto markup = std::string {
        R"(<linearGradient>)"
        R"(<stop offset="0" stop-color="#ff0000"/>)"
        R"(<stop offset="50%" style="stop-color:#00ff00;stop-opacity:0.5"/>)"
        R"(<stop offset="1" stop-color="#0000ff" stop-opacity="0.25"/>)"
        R"(</linearGradient>)"};

    auto stops = SVG::parseGradientStops(documentFrom(markup));

    check(stops.size() == 3);

    check(isNear(stops[0].position, 0.f) && isNear(stops[0].color.r, 1.f));
    check(isNear(stops[1].position, 0.5f), "a percentage offset is a fraction");
    check(isNear(stops[1].color.g, 1.f) && isNear(stops[1].color.a, 0.5f),
          "a style declaration carries the colour and the opacity");
    check(isNear(stops[2].color.b, 1.f) && isNear(stops[2].color.a, 0.25f));
};

// The default units, which is what nearly every document uses by saying nothing.
// The same markup has to come out placed differently on two different shapes.
auto tBoundingBoxUnits =
    test("SVGGradient/boundingBoxUnitsFollowTheShapeTheyPaint") = []
{
    auto document = documentFrom(
        R"(<svg><linearGradient id="g"><stop offset="0" stop-color="red"/>)"
        R"(<stop offset="1" stop-color="blue"/></linearGradient></svg>)");

    auto byId = SVG::ElementsById {};
    SVG::collectIds(document, byId);

    auto wide =
        SVG::resolveGradient("g", byId, {10.f, 20.f, 100.f, 40.f}, anyViewport, {});

    // x1/y1/x2/y2 default to 0%, 0%, 100%, 0%: the box's left edge to its right.
    check(isNear(endOf(wide, false), 10.f, 20.f));
    check(isNear(endOf(wide, true), 110.f, 20.f));

    auto tall =
        SVG::resolveGradient("g", byId, {0.f, 0.f, 20.f, 300.f}, anyViewport, {});

    check(isNear(endOf(tall, true), 20.f, 0.f), "the same markup, another box");
};

auto tUserSpaceUnits = test("SVGGradient/userSpaceUnitsIgnoreTheShape") = []
{
    auto document = documentFrom(
        R"(<svg><linearGradient id="g" gradientUnits="userSpaceOnUse")"
        R"( x1="30" y1="5" x2="70" y2="5">)"
        R"(<stop offset="0" stop-color="red"/></linearGradient></svg>)");

    auto byId = SVG::ElementsById {};
    SVG::collectIds(document, byId);

    auto gradient =
        SVG::resolveGradient("g", byId, {1000.f, 1000.f, 1.f, 1.f}, anyViewport, {});

    check(isNear(endOf(gradient, false), 30.f, 5.f));
    check(isNear(endOf(gradient, true), 70.f, 5.f));
};

auto tGradientTransformIsInsideTheBox =
    test("SVGGradient/gradientTransformAppliesInsideTheBox") = []
{
    auto document = documentFrom(
        R"X(<svg><linearGradient id="g" gradientTransform="translate(0.5 0)">)X"
        R"(<stop offset="0" stop-color="red"/></linearGradient></svg>)");

    auto byId = SVG::ElementsById {};
    SVG::collectIds(document, byId);

    auto gradient =
        SVG::resolveGradient("g", byId, {0.f, 0.f, 200.f, 100.f}, anyViewport, {});

    // Half a *box* across and not half a user unit: the translate is written in
    // the box's own fractions, so it comes out as 100 points rather than 0.5.
    check(isNear(endOf(gradient, false), 100.f, 0.f));
};

auto tElementTransformApplies =
    test("SVGGradient/theElementsOwnTransformMapsTheResult") = []
{
    auto document = documentFrom(
        R"(<svg><linearGradient id="g" gradientUnits="userSpaceOnUse")"
        R"( x1="0" y1="0" x2="10" y2="0">)"
        R"(<stop offset="0" stop-color="red"/></linearGradient></svg>)");

    auto byId = SVG::ElementsById {};
    SVG::collectIds(document, byId);

    auto scaled = GPUWidgets::AffineTransform::scaling(3.f, 3.f);

    auto gradient =
        SVG::resolveGradient("g", byId, {0.f, 0.f, 1.f, 1.f}, anyViewport, scaled);

    check(isNear(endOf(gradient, true), 30.f, 0.f));
};

// How every drawing program writes a family of gradients: one carries the stops
// and the rest point at it. A reader that does not follow href draws all of them
// as nothing, and says nothing about why.
auto tHrefInheritance = test("SVGGradient/hrefCarriesStopsAndAttributes") = []
{
    auto document =
        documentFrom(R"(<svg>)"
                     R"(<linearGradient id="base" spreadMethod="reflect">)"
                     R"(<stop offset="0" stop-color="#ff0000"/>)"
                     R"(<stop offset="1" stop-color="#0000ff"/></linearGradient>)"
                     R"(<linearGradient id="child" href="#base" x2="0" y2="1"/>)"
                     R"(<linearGradient id="legacy" xlink:href="#base"/>)"
                     R"(</svg>)");

    auto byId = SVG::ElementsById {};
    SVG::collectIds(document, byId);

    auto child = SVG::resolveGradient("child", byId, anyBox, anyViewport, {});

    check(child.stops.size() == 2, "stops come through the reference");
    check(child.spread == UI::GradientSpread::Reflect,
          "and so does anything else the child does not say for itself");
    check(isNear(endOf(child, true), 0.f, 10.f), "what the child does say wins");

    auto legacy = SVG::resolveGradient("legacy", byId, anyBox, anyViewport, {});

    check(legacy.stops.size() == 2, "xlink:href is the same attribute");
};

// A cycle is a document nobody meant to write and every reader has to survive.
auto tHrefCycle = test("SVGGradient/anHrefCycleTerminates") = []
{
    auto document = documentFrom(R"(<svg>)"
                                 R"(<linearGradient id="a" href="#b"/>)"
                                 R"(<linearGradient id="b" href="#a"/>)"
                                 R"(</svg>)");

    auto byId = SVG::ElementsById {};
    SVG::collectIds(document, byId);

    check(SVG::resolveGradient("a", byId, anyBox, anyViewport, {}).isEmpty());
};

auto tRadialDefaults = test("SVGGradient/aRadialCentresItselfOnTheBox") = []
{
    auto document = documentFrom(
        R"(<svg><radialGradient id="g">)"
        R"(<stop offset="0" stop-color="red"/></radialGradient></svg>)");

    auto byId = SVG::ElementsById {};
    SVG::collectIds(document, byId);

    auto gradient =
        SVG::resolveGradient("g", byId, {0.f, 0.f, 80.f, 40.f}, anyViewport, {});

    check(gradient.kind == UI::Gradient::Kind::Radial);

    // cx, cy and r all default to 50%, so the centre is the box's own.
    check(isNear(endOf(gradient, false), 40.f, 20.f));
    check(isNear(gradient.radius, 0.5f), "and the radius stays in the box's units");
};

// A percentage in user space is a percentage of the viewport, which is the one
// place the two unit systems need different arithmetic rather than the same
// arithmetic on different numbers.
auto tUserSpacePercentages =
    test("SVGGradient/aPercentageInUserSpaceIsOfTheViewport") = []
{
    auto document = documentFrom(
        R"(<svg><linearGradient id="g" gradientUnits="userSpaceOnUse")"
        R"( x1="0%" y1="0%" x2="50%" y2="0%">)"
        R"(<stop offset="0" stop-color="red"/></linearGradient></svg>)");

    auto byId = SVG::ElementsById {};
    SVG::collectIds(document, byId);

    auto gradient =
        SVG::resolveGradient("g", byId, anyBox, {0.f, 0.f, 400.f, 100.f}, {});

    check(isNear(endOf(gradient, true), 200.f, 0.f));
};

// Every one of these is a document that still renders, so each has to fall back
// to the colour written beside the reference rather than to nothing.
auto tGradientRefusals = test("SVGGradient/aReferenceThatResolvesToNothing") = []
{
    auto document = documentFrom(
        R"(<svg><linearGradient id="empty"/><rect id="notAGradient"/></svg>)");

    auto byId = SVG::ElementsById {};
    SVG::collectIds(document, byId);

    check(SVG::resolveGradient("missing", byId, anyBox, anyViewport, {}).isEmpty());
    check(SVG::resolveGradient("empty", byId, anyBox, anyViewport, {}).isEmpty(),
          "a gradient with no stops paints nothing and is not one");
    check(SVG::resolveGradient("notAGradient", byId, anyBox, anyViewport, {})
              .isEmpty());
};

auto tSpreadMethod = test("SVGGradient/spreadMethodHasPadForADefault") = []
{
    check(SVG::parseSpreadMethod("") == UI::GradientSpread::Pad);
    check(SVG::parseSpreadMethod("pad") == UI::GradientSpread::Pad);
    check(SVG::parseSpreadMethod("reflect") == UI::GradientSpread::Reflect);
    check(SVG::parseSpreadMethod("repeat") == UI::GradientSpread::Repeat);
    check(SVG::parseSpreadMethod("nonsense") == UI::GradientSpread::Pad);
};

// The placement only means something if it can be undone: what reaches the
// shader is its inverse, and a matrix that cannot be inverted is a gradient that
// would divide by nothing in every fragment of the shape.
auto tInverted = test("AffineTransform/invertedUndoesTheTransform") = []
{
    auto transform = GPUWidgets::AffineTransform::scaling(2.f, 4.f)
                         .then(GPUWidgets::AffineTransform::rotation(0.7f))
                         .then(GPUWidgets::AffineTransform::translation(11.f, -3.f));

    auto point = Graphics::Point {13.f, 29.f};
    auto back = transform.inverted().apply(transform.apply(point));

    check(isNear(back, point.x, point.y));

    // A matrix that collapsed the plane has no inverse, and hands back the
    // identity rather than infinities.
    check(GPUWidgets::AffineTransform::scaling(1.f, 0.f).inverted().isIdentity());
};
