#include <eacp/SVG/SVG.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

// The parse layer and the geometry it builds, with no renderer involved.
//
// The module had no tests at all before the component tier used it, and what it
// got wrong was exactly what nothing was watching: transform lists that did not
// compose, a viewBox origin read and then discarded, presentation attributes
// that did not inherit. Each of those is a fact about a matrix or a rectangle,
// so each can be pinned here without a window, a device or a frame.
//
// The same holds of everything rung 2 added -- how a viewBox is fitted, what an
// arc's four flag combinations pick out, which of style="" and an attribute
// wins, where a <use> puts what it names. All of it is arithmetic on points, and
// all of it is the kind of thing that draws *nearly* right when it is wrong.

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

// Laid out before anything is asked of it: geometry is built against the
// component's size, so one that was never sized has nothing in it.
OwningPointer<SVG::SVGComponent> componentFor(const std::string& markup,
                                              float width = 100.f,
                                              float height = 100.f)
{
    auto component = makeOwned<SVG::SVGComponent>();

    component->setBounds({0.f, 0.f, width, height});
    component->setDocument(documentFrom(markup));

    return component;
}
} // namespace

// The claim the template makes is not that the two path types agree point for
// point - a native CGPath or Direct2D geometry cannot be read back - but that
// one parser body drives both.
auto tBothPathTypes = test("SVGPathParser/buildsEitherPathTypeFromOneBody") = []
{
    auto d = std::string {"M 10 10 L 90 10 Q 90 50 50 50 C 30 50 10 30 10 10 Z"};

    auto gpu = SVG::parseSVGPath<GPUWidgets::Path>(d);
    auto native = SVG::parseSVGPath<Graphics::Path>(d);

    check(!gpu.isEmpty());
    check(gpu.getSubPaths().size() == 1);
    check(gpu.getSubPaths()[0].closed, "Z should close the sub-path");

    check(native.scaled(2.f, 2.f).getHandle() != nullptr);
};

auto tRelativeCommands = test("SVGPathParser/relativeCommandsAccumulate") = []
{
    auto bounds =
        SVG::parseSVGPath<GPUWidgets::Path>("M 10 10 h 20 v 20 h -20 z").getBounds();

    check(isNear(bounds.x, 10.f));
    check(isNear(bounds.y, 10.f));
    check(isNear(bounds.w, 20.f));
    check(isNear(bounds.h, 20.f));
};

auto tSecondSubPath = test("SVGPathParser/aSecondMoveToStartsASecondSubPath") = []
{
    auto path = SVG::parseSVGPath<GPUWidgets::Path>("M 0 0 h 10 z M 100 100 h 10 z");

    check(path.getSubPaths().size() == 2);
    check(isNear(path.getSubPaths()[1].points[0], 100.f, 100.f));
};

// Why parseSVGPathInto appends rather than returns: a path about to be scaled
// up, or about to be stroked, needs a tighter tolerance than the default, and
// only the caller knows which. A returning parser gives it no way to say.
auto tFlatnessIsTheCallers =
    test("SVGPathParser/flatnessSetBeforehandDecidesTheSegmentCount") = []
{
    auto d = std::string {"M 0 0 C 0 100 100 100 100 0"};

    auto loose = GPUWidgets::Path {};
    loose.setFlatness(1.f);
    SVG::parseSVGPathInto(d, loose);

    auto tight = GPUWidgets::Path {};
    tight.setFlatness(0.01f);
    SVG::parseSVGPathInto(d, tight);

    check(tight.getSubPaths()[0].points.size()
              > loose.getSubPaths()[0].points.size(),
          "a tighter tolerance has to buy more segments");
};

// The bug the matrix form exists to fix. translate then rotate means rotate
// first, because the translate moves the coordinate system the rotate is written
// in - and the struct-of-fields parseTransform cannot tell this from its
// reverse, since both leave the same translateX and the same rotateDeg.
auto tListComposes = test("SVGTransform/aListComposesRightToLeft") = []
{
    auto forward = SVG::parseTransformMatrix("translate(100 0) rotate(90)");
    auto reversed = SVG::parseTransformMatrix("rotate(90) translate(100 0)");

    check(isNear(forward.apply({1.f, 0.f}), 100.f, 1.f));
    check(isNear(reversed.apply({1.f, 0.f}), 0.f, 101.f));

    auto flattened = SVG::parseTransform("translate(100 0) rotate(90)");
    auto flattenedReverse = SVG::parseTransform("rotate(90) translate(100 0)");

    check(flattened.translateX == flattenedReverse.translateX
              && flattened.rotateDeg == flattenedReverse.rotateDeg,
          "the flattened form is expected to lose the ordering");
};

auto tRepeatedFunction = test("SVGTransform/aRepeatedFunctionAccumulates") = []
{
    check(isNear(SVG::parseTransformMatrix("translate(10 0) translate(5 3)")
                     .apply({0.f, 0.f}),
                 15.f,
                 3.f));
};

auto tRotateAbout = test("SVGTransform/rotateAboutAPointLeavesThatPointAlone") = []
{
    check(isNear(SVG::parseTransformMatrix("rotate(37 50 60)").apply({50.f, 60.f}),
                 50.f,
                 60.f));
};

auto tEveryFunction = test("SVGTransform/matrixSkewAndScaleAreAllRead") = []
{
    check(
        isNear(SVG::parseTransformMatrix("matrix(2 0 0 3 10 20)").apply({1.f, 1.f}),
               12.f,
               23.f));

    // skewX shifts x by y and leaves y alone.
    check(
        isNear(SVG::parseTransformMatrix("skewX(45)").apply({0.f, 2.f}), 2.f, 2.f));

    // One argument to scale means both axes.
    check(isNear(SVG::parseTransformMatrix("scale(3)").apply({1.f, 2.f}), 3.f, 6.f));
};

// What a stroke width is multiplied by and what the flatness is divided by, so
// it decides both how thick a scaled stroke comes out and how finely a scaled
// curve is flattened.
auto tScaleFactor = test("SVGTransform/theScaleFactorIsTheRootOfTheAreaScale") = []
{
    check(isNear(SVG::parseTransformMatrix("scale(4)").getScaleFactor(), 4.f));
    check(isNear(SVG::parseTransformMatrix("rotate(31)").getScaleFactor(), 1.f));
    check(isNear(SVG::parseTransformMatrix("scale(2 8)").getScaleFactor(), 4.f));
};

auto tThenOrder = test("AffineTransform/thenAppliesTheReceiverFirst") = []
{
    auto move = GPUWidgets::AffineTransform::translation(10.f, 0.f);
    auto grow = GPUWidgets::AffineTransform::scaling(2.f, 2.f);

    check(isNear(move.then(grow).apply({0.f, 0.f}), 20.f, 0.f));
    check(isNear(grow.then(move).apply({0.f, 0.f}), 10.f, 0.f));
};

auto tTransformed =
    test("Path/transformedMapsEveryPointAndLeavesTheSourceAlone") = []
{
    auto path = GPUWidgets::Path {};
    path.addRect({0.f, 0.f, 10.f, 20.f});

    auto bounds =
        path.transformed(GPUWidgets::AffineTransform::translation(5.f, 7.f))
            .getBounds();

    check(isNear(bounds.x, 5.f));
    check(isNear(bounds.y, 7.f));
    check(isNear(bounds.w, 10.f));
    check(isNear(bounds.h, 20.f));

    // Untouched, which is what lets a builder hold one path and place it twice.
    check(isNear(path.getBounds().x, 0.f));
};

auto tScaled = test("Path/scaledIsTransformedByAScaling") = []
{
    auto path = GPUWidgets::Path {};
    path.addRect({1.f, 2.f, 3.f, 4.f});

    auto bounds = path.scaled(2.f, 3.f).getBounds();

    check(isNear(bounds.x, 2.f));
    check(isNear(bounds.y, 6.f));
    check(isNear(bounds.w, 6.f));
    check(isNear(bounds.h, 12.f));
};

auto tColours = test("SVGAttributes/colourFormsAndNone") = []
{
    check(SVG::parseColor("none").isNone);
    check(SVG::parseColor("").isNone);

    auto hex = SVG::parseColor("#4A90D9");
    check(!hex.isNone);
    check(isNear(hex.color.r, 74.f / 255.f));

    check(isNear(SVG::parseColor("#f00").color.r, 1.f));
    check(isNear(SVG::parseColor("#f00").color.g, 0.f));
    check(isNear(SVG::parseColor("rgb(255, 128, 0)").color.g, 128.f / 255.f));
    check(isNear(SVG::parseColor("white").color.b, 1.f));
};

auto tXML = test("XMLParser/readsTagsAttributesAndText") = []
{
    auto root = documentFrom(
        R"(<svg width="10"><g fill="red"><rect x="1"/></g><text>hi</text></svg>)");

    check(root.tag == "svg");
    check(root.children.size() == 2);
    check(root.children[0].attr("fill") == "red");
    check(isNear(root.children[0].children[0].numAttr("x"), 1.f));
    check(root.children[1].textContent == "hi");
};

// SVGBuilder reads the third and fourth numbers of a viewBox and nothing else,
// so a document with a non-zero origin renders shifted by it and says nothing.
auto tViewBoxOrigin =
    test("SVGComponent/readsTheViewBoxOriginRatherThanDiscardingIt") = []
{
    auto component = componentFor(
        R"(<svg viewBox="10 20 100 200"><rect x="10" y="20" width="1" height="1"/></svg>)");

    auto box = component->getViewBox();

    check(isNear(box.x, 10.f), "the origin is part of the viewBox");
    check(isNear(box.y, 20.f));
    check(isNear(box.w, 100.f));
    check(isNear(box.h, 200.f));

    // With no width or height of its own, the viewBox is the intrinsic size.
    check(isNear(component->getDocumentWidth(), 100.f));
    check(isNear(component->getDocumentHeight(), 200.f));
};

auto tNoViewBox = test("SVGComponent/aMissingViewBoxFallsBackToTheDocumentSize") = []
{
    auto component = componentFor(R"(<svg width="300" height="150"></svg>)");

    check(isNear(component->getViewBox().w, 300.f));
    check(isNear(component->getViewBox().h, 150.f));
};

// A PathShape holds one filled region, and a stroke is a different region, so
// the two cannot share one. Which is also the cost: every stroked-and-filled
// element doubles what it asks the atlas for.
auto tTwoMasks = test("SVGComponent/anElementFilledAndStrokedIsTwoMasks") = []
{
    auto both = componentFor(
        R"(<svg width="100" height="100"><rect x="10" y="10" width="50" height="50" fill="red" stroke="blue" stroke-width="2"/></svg>)");

    check(both->getShapeCount() == 2);

    auto fillOnly = componentFor(
        R"(<svg width="100" height="100"><rect x="10" y="10" width="50" height="50" fill="red"/></svg>)");

    check(fillOnly->getShapeCount() == 1);
};

// A group's fill has to reach its children, or a document that sets fill once on
// a `g` - which is most of them - comes out black throughout. Nothing here can
// read a colour off a mask, so the observable is that fill="none" on the group
// leaves the child with no fill mask at all.
auto tInheritance =
    test("SVGComponent/presentationAttributesInheritFromTheGroup") = []
{
    auto inherited = componentFor(
        R"(<svg width="100" height="100"><g fill="none" stroke="red" stroke-width="1"><circle cx="50" cy="50" r="20"/></g></svg>)");

    check(inherited->getShapeCount() == 1,
          "stroke only: the group's fill reached it");

    auto overridden = componentFor(
        R"(<svg width="100" height="100"><g fill="none"><circle cx="50" cy="50" r="20" fill="green"/></g></svg>)");

    check(overridden->getShapeCount() == 1, "the child's own fill wins");

    auto neither = componentFor(
        R"(<svg width="100" height="100"><g fill="none"><circle cx="50" cy="50" r="20"/></g></svg>)");

    check(neither->getShapeCount() == 0, "nothing to fill and nothing to stroke");
};

auto tNeedsALayout = test("SVGComponent/aDocumentNeverLaidOutBuildsNothing") = []
{
    auto component = SVG::SVGComponent {};

    component.setDocument(documentFrom(
        R"(<svg width="100" height="100"><rect x="0" y="0" width="10" height="10"/></svg>)"));

    check(component.getShapeCount() == 0, "no size to build against yet");

    component.setBounds({0.f, 0.f, 100.f, 100.f});
    check(component.getShapeCount() == 1, "a resize is what builds it");
};

auto tFontsAreShared = test("SVGComponent/oneRendererPerFamilyAndSize") = []
{
    auto component = componentFor(R"(<svg width="200" height="100">)"
                                  R"(<text x="0" y="20" font-size="12">a</text>)"
                                  R"(<text x="0" y="40" font-size="12">b</text>)"
                                  R"(<text x="0" y="60" font-size="24">c</text>)"
                                  R"(</svg>)",
                                  200.f,
                                  100.f);

    check(component->getFontCount() == 2, "two sizes, not three strings");
    check(component->getShapeCount() == 0, "text is not a mask");
};

// The measurement rung 1 exists to take, in miniature. Two documents the same
// size on screen, one tiling and one stacking: the tiled one's masks add up to
// about the document's own area, the stacked one's to many times it, and the
// atlas only ever sees the sum. plan.md's ceiling argument bounds the first and
// says nothing about the second.
auto tMaskAreaFollowsTheShapes =
    test("SVGComponent/maskAreaGrowsWithTheShapesAndNotTheWindow") = []
{
    auto tiled = std::string {R"(<svg width="100" height="100">)"};
    auto stacked = tiled;

    for (auto i = 0; i < 16; ++i)
    {
        tiled += R"(<rect x=")" + std::to_string((i % 4) * 25) + R"(" y=")"
                 + std::to_string((i / 4) * 25)
                 + R"(" width="25" height="25" fill="red"/>)";

        stacked += R"(<rect x="0" y="0" width="100" height="100" fill="red"/>)";
    }

    auto tiledDocument = componentFor(tiled + "</svg>");
    auto stackedDocument = componentFor(stacked + "</svg>");

    check(tiledDocument->getShapeCount() == 16);
    check(stackedDocument->getShapeCount() == 16);

    auto documentArea = 100.f * 100.f;

    check(tiledDocument->getTotalMaskArea() < 1.1f * documentArea,
          "tiled shapes cannot exceed what they tile");
    check(stackedDocument->getTotalMaskArea() > 15.f * documentArea,
          "stacked ones are bounded by nothing");
};

// --------------------------------------------------------- preserveAspectRatio

// The default is the point. A document that says nothing about its aspect gets
// xMidYMid meet, and the fit that falls out of scaling each axis to its own -
// which is what the module did - is the one value the format calls "none".
auto tAspectDefault = test("PreserveAspectRatio/theDefaultIsUniformAndCentred") = []
{
    auto fit = SVG::parsePreserveAspectRatio("");

    check(fit.uniform);
    check(!fit.slice);
    check(fit.x == SVG::PreserveAspectRatio::Align::Mid);
    check(fit.y == SVG::PreserveAspectRatio::Align::Mid);

    // A 200x100 box in a 100x100 viewport: uniform means both axes at 0.5, and
    // centred means the 50 points of spare height are split.
    auto transform = SVG::viewBoxTransform(
        {0.f, 0.f, 200.f, 100.f}, {0.f, 0.f, 100.f, 100.f}, fit);

    check(isNear(transform.apply({0.f, 0.f}), 0.f, 25.f));
    check(isNear(transform.apply({200.f, 100.f}), 100.f, 75.f));
};

auto tAspectNone = test("PreserveAspectRatio/noneStretchesEachAxisToItsOwn") = []
{
    auto fit = SVG::parsePreserveAspectRatio("none");

    check(!fit.uniform);

    auto transform = SVG::viewBoxTransform(
        {0.f, 0.f, 200.f, 100.f}, {0.f, 0.f, 100.f, 100.f}, fit);

    check(isNear(transform.apply({200.f, 100.f}), 100.f, 100.f),
          "the far corner reaches the far corner, which is what distorts");
};

auto tAspectSlice = test("PreserveAspectRatio/sliceCoversAndMeetFits") = []
{
    auto box = Graphics::Rect {0.f, 0.f, 200.f, 100.f};
    auto viewport = Graphics::Rect {0.f, 0.f, 100.f, 100.f};

    auto meet = SVG::viewBoxTransform(
        box, viewport, SVG::parsePreserveAspectRatio("xMidYMid meet"));
    auto slice = SVG::viewBoxTransform(
        box, viewport, SVG::parsePreserveAspectRatio("xMidYMid slice"));

    // meet takes the smaller scale and leaves the viewport's spare axis empty;
    // slice takes the larger and overflows, which the component's clip cuts.
    check(isNear(meet.getScaleFactor(), 0.5f));
    check(isNear(slice.getScaleFactor(), 1.f));

    check(isNear(slice.apply({0.f, 0.f}), -50.f, 0.f),
          "the overflow is split, so the box starts before the viewport does");
};

auto tAspectAlignment =
    test("PreserveAspectRatio/alignmentDecidesWhereTheSpareGoes") = []
{
    auto box = Graphics::Rect {0.f, 0.f, 200.f, 100.f};
    auto viewport = Graphics::Rect {0.f, 0.f, 100.f, 100.f};

    auto atOrigin = SVG::viewBoxTransform(
        box, viewport, SVG::parsePreserveAspectRatio("xMinYMin meet"));
    auto atFarCorner = SVG::viewBoxTransform(
        box, viewport, SVG::parsePreserveAspectRatio("xMaxYMax meet"));

    check(isNear(atOrigin.apply({0.f, 0.f}), 0.f, 0.f));
    check(isNear(atFarCorner.apply({0.f, 0.f}), 0.f, 50.f));
};

auto tAspectFromTheDocument =
    test("SVGComponent/aDocumentIsFittedRatherThanStretched") = []
{
    auto component = componentFor(
        R"(<svg viewBox="0 0 200 100"><rect x="0" y="0" width="200" height="100"/></svg>)",
        100.f,
        100.f);

    auto transform = component->documentToComponent();

    check(component->getAspectRatio().uniform);
    check(isNear(transform.apply({0.f, 0.f}), 0.f, 25.f),
          "letterboxed, not stretched to the component's own aspect");

    auto stretched = componentFor(
        R"(<svg viewBox="0 0 200 100" preserveAspectRatio="none"><rect x="0" y="0" width="200" height="100"/></svg>)",
        100.f,
        100.f);

    check(isNear(stretched->documentToComponent().apply({0.f, 0.f}), 0.f, 0.f));
};

// ------------------------------------------------------------------ style=""

auto tStyleDeclarations =
    test("SVGAttributes/styleDeclarationsAreReadAsProperties") = []
{
    auto declarations =
        SVG::parseStyleDeclarations("  fill : #ff0000 ; stroke-width:2.5;  ");

    check(declarations.size() == 2, "the trailing semicolon is not a third");
    check(declarations["fill"] == "#ff0000", "the value is trimmed");
    check(declarations["stroke-width"] == "2.5");
};

// The one bit of the cascade a document can rely on without a stylesheet, and
// the reason it matters: drawing programs emit both spellings, so reading the
// attribute and ignoring the declaration means reading whichever the program
// wrote for compatibility rather than what it meant.
auto tStyleBeatsTheAttribute =
    test("SVGComponent/aStyleDeclarationBeatsTheAttributeOfTheSameName") = []
{
    auto declared = componentFor(
        R"(<svg width="100" height="100"><rect x="10" y="10" width="50" height="50" fill="red" style="fill:none"/></svg>)");

    check(declared->getShapeCount() == 0, "the declaration said none");

    auto attributeOnly = componentFor(
        R"(<svg width="100" height="100"><rect x="10" y="10" width="50" height="50" fill="red"/></svg>)");

    check(attributeOnly->getShapeCount() == 1);
};

auto tStyleInherits =
    test("SVGComponent/styleDeclarationsInheritLikeAttributes") = []
{
    auto component = componentFor(
        R"(<svg width="100" height="100"><g style="fill:none;stroke:red;stroke-width:1"><circle cx="50" cy="50" r="20"/></g></svg>)");

    check(component->getShapeCount() == 1,
          "the group's declarations reached the child");
};

// ---------------------------------------------------------------------- arcs

// Four arcs join any two points at a given pair of radii, and the two flags are
// what pick one. Getting the pair the wrong way round draws a shape that is
// still an arc between the right endpoints, which is why this is checked by
// where the bulge went rather than by whether anything was drawn.
auto tArcFlagsPickTheArc = test("SVGPathParser/theTwoArcFlagsPickOneOfFourArcs") = []
{
    auto boundsOf = [](const std::string& d)
    { return SVG::parseSVGPath<GPUWidgets::Path>(d).getBounds(); };

    // A half circle from (0,50) to (100,50). Sweep bulges it one way, and its
    // absence the other; in SVG's y-down space sweep=1 is the screen's
    // clockwise, so the bulge goes up.
    auto up = boundsOf("M 0 50 A 50 50 0 0 1 100 50");
    auto down = boundsOf("M 0 50 A 50 50 0 0 0 100 50");

    check(isNear(up.y, 0.f) && isNear(up.h, 50.f));
    check(isNear(down.y, 50.f) && isNear(down.h, 50.f));

    // The large-arc flag on a quarter turn takes the three quarters instead, so
    // the same endpoints enclose the whole circle's extent.
    auto minor = boundsOf("M 50 0 A 50 50 0 0 1 100 50");
    auto major = boundsOf("M 50 0 A 50 50 0 1 1 100 50");

    check(isNear(minor.w, 50.f) && isNear(minor.h, 50.f));
    check(isNear(major.w, 100.f) && isNear(major.h, 100.f));
};

// Documents get this wrong constantly, usually by rounding the radii down, and
// the specification says to grow them rather than to drop the arc.
auto tArcRadiiAreGrown = test("SVGPathParser/radiiTooSmallToReachAreGrown") = []
{
    auto path = SVG::parseSVGPath<GPUWidgets::Path>("M 0 0 A 1 1 0 0 1 100 0");
    auto bounds = path.getBounds();

    check(isNear(bounds.w, 100.f), "it still has to arrive at the endpoint");
    check(bounds.h > 40.f, "grown to a radius that just reaches, which is 50");
};

auto tArcDegenerate = test("SVGPathParser/aZeroRadiusArcIsTheLineToItsEndpoint") = []
{
    auto path = SVG::parseSVGPath<GPUWidgets::Path>("M 0 0 A 0 0 0 0 1 100 40");
    auto bounds = path.getBounds();

    check(path.getSubPaths().size() == 1, "the sub-path stays connected");
    check(isNear(bounds.w, 100.f) && isNear(bounds.h, 40.f));
};

// The flags are single characters and the grammar lets them run into what
// follows, which is how every minifier writes them. Read as numbers, "0150"
// is one value and every coordinate after it lands somewhere else.
auto tArcFlagsNeedNoSeparator =
    test("SVGPathParser/arcFlagsMayBeWrittenWithNoSeparator") = []
{
    auto packed =
        SVG::parseSVGPath<GPUWidgets::Path>("M0 0a50 50 0 0150 50").getBounds();
    auto spaced =
        SVG::parseSVGPath<GPUWidgets::Path>("M0 0 a50 50 0 0 1 50 50").getBounds();

    check(isNear(packed.x, spaced.x) && isNear(packed.y, spaced.y));
    check(isNear(packed.w, spaced.w) && isNear(packed.h, spaced.h));
    check(isNear(packed.w, 50.f),
          "and it is the arc that was written, not 110 of them");
};

// An arc is emitted as cubics rather than as a polyline, which is what lets one
// parser body serve a native path and the GPU one. The GPU one then flattens
// them to whatever tolerance it was told to hold.
auto tArcHonoursFlatness =
    test("SVGPathParser/anArcIsFlattenedToThePathsTolerance") = []
{
    auto d = std::string {"M 0 50 A 50 50 0 0 1 100 50"};

    auto loose = GPUWidgets::Path {};
    loose.setFlatness(2.f);
    SVG::parseSVGPathInto(d, loose);

    auto tight = GPUWidgets::Path {};
    tight.setFlatness(0.01f);
    SVG::parseSVGPathInto(d, tight);

    check(tight.getSubPaths()[0].points.size()
          > loose.getSubPaths()[0].points.size());
};

// ------------------------------------------------------------- defs and use

auto tDefsDrawNothing = test("SVGComponent/defsHoldGeometryAndDrawNone") = []
{
    auto component = componentFor(
        R"(<svg width="100" height="100"><defs><rect id="box" x="0" y="0" width="10" height="10" fill="red"/></defs></svg>)");

    check(component->getShapeCount() == 0);
};

auto tUseInstantiates = test("SVGComponent/useDrawsWhatItNamesWhereItSaysTo") = []
{
    auto component = componentFor(
        R"(<svg width="100" height="100"><defs><rect id="box" x="0" y="0" width="10" height="10" fill="red"/></defs>)"
        R"(<use href="#box" x="40" y="40"/><use href="#box" x="80" y="80"/></svg>)");

    check(component->getShapeCount() == 2,
          "two use sites are two masks -- each has its own transform");

    // The document is 100x100 in a 100x100 component, so document units are
    // component points and the second copy's mask sits where x/y put it.
    check(component->getTotalMaskArea() > 0.f);
};

auto tUseOfAMissingId = test("SVGComponent/useOfSomethingAbsentDrawsNothing") = []
{
    check(
        componentFor(
            R"(<svg width="100" height="100"><use href="#gone" x="10" y="10"/></svg>)")
            ->getShapeCount()
        == 0);

    check(componentFor(R"(<svg width="100" height="100"><use x="10"/></svg>)")
              ->getShapeCount()
          == 0);
};

// The referenced content inherits from the use site rather than from where it
// was written, which is the whole reason a document keeps one shape in defs and
// draws it in six colours.
auto tUseInheritsFromTheUseSite =
    test("SVGComponent/referencedContentInheritsFromTheUseSite") = []
{
    auto component = componentFor(
        R"(<svg width="100" height="100"><defs><rect id="box" x="0" y="0" width="10" height="10"/></defs>)"
        R"(<use href="#box" fill="none"/></svg>)");

    check(component->getShapeCount() == 0, "the use site's fill:none reached it");
};

// Forbidden by the format and perfectly writable, so the walk has to stop by
// itself rather than by being told the document is well formed.
auto tUseCycleTerminates =
    test("SVGComponent/aUseCycleStopsRatherThanRecursing") = []
{
    auto component = componentFor(
        R"(<svg width="100" height="100"><g id="loop"><rect x="0" y="0" width="10" height="10" fill="red"/><use href="#loop"/></g></svg>)");

    check(component->getShapeCount() > 0 && component->getShapeCount() < 32,
          "it drew, and it stopped");
};

auto tSymbolIsFittedToTheUse =
    test("SVGComponent/aSymbolIsFittedToTheSizeTheUseAsksFor") = []
{
    auto component = componentFor(
        R"(<svg width="100" height="100"><defs><symbol id="s" viewBox="0 0 10 10"><rect x="0" y="0" width="10" height="10" fill="red"/></symbol></defs>)"
        R"(<use href="#s" x="0" y="0" width="50" height="50"/></svg>)");

    check(component->getShapeCount() == 1);

    // The symbol's own 10x10 box, drawn at the 50x50 the use asked for.
    check(component->getTotalMaskArea() > 2000.f
              && component->getTotalMaskArea() < 3000.f,
          "the symbol's viewBox was mapped onto the use's size");
};

// A sprite sheet is one document of nested <svg> icons, each with a viewBox
// of its own and an x, y, width and height placing it in the sheet. Met in
// the tree rather than through a <use>, one was drawn as a plain group: its
// viewBox ignored, its content at whatever scale it was authored in.
auto tNestedSvgIsAViewport =
    test("SVGComponent/aNestedSvgIsFittedToItsOwnWidthAndHeight") = []
{
    auto component = componentFor(
        R"(<svg width="100" height="100"><svg x="0" y="50" width="50" height="50" viewBox="0 0 1000 1000">)"
        R"(<rect x="0" y="0" width="1000" height="1000" fill="red"/></svg></svg>)");

    check(component->getShapeCount() == 1);

    // The icon's 1000x1000 box, drawn at the 50x50 it was given.
    check(component->getTotalMaskArea() > 2000.f
              && component->getTotalMaskArea() < 3000.f,
          "the nested svg's viewBox was mapped onto its width and height");
};

// ----------------------------------------------------------------- dashing

// The observable here is thin on purpose: a dashed stroke is still one region
// and therefore one mask, so what a component can say is that it drew and that
// dashing did not turn one element into many. What the dashes actually are is
// pinned in the GPUWidgets tests, where the geometry is readable.
auto tDashedStrokeIsOneMask = test("SVGComponent/aDashedStrokeIsStillOneMask") = []
{
    auto component = componentFor(
        R"(<svg width="100" height="100"><path d="M 10 50 L 90 50" fill="none" stroke="red" stroke-width="4" stroke-dasharray="6 3"/></svg>)");

    check(component->getShapeCount() == 1);

    auto solid = componentFor(
        R"(<svg width="100" height="100"><path d="M 10 50 L 90 50" fill="none" stroke="red" stroke-width="4"/></svg>)");

    check(component->getTotalMaskArea() > 0.f);
    check(component->getTotalMaskArea() <= solid->getTotalMaskArea() + tolerance,
          "cutting a line up cannot make the region it covers any bigger");
};

// -------------------------------------------------------------- font cache

// A rebuild happens on every resize, and each renderer it drops is a glyph atlas
// to raster again. Keeping them is only half the fix: the point size is the
// document's times the transform's scale, so a resize genuinely asks for new
// sizes and a cache that only ever grew would end a drag holding one renderer
// per frame of it.
auto tFontsSurviveARebuild =
    test("SVGComponent/theFontCacheIsExactlyWhatTheLastBuildUsed") = []
{
    auto markup =
        std::string {R"(<svg width="200" height="100" viewBox="0 0 200 100">)"
                     R"(<text x="0" y="20" font-size="12">a</text>)"
                     R"(<text x="0" y="60" font-size="24">b</text>)"
                     R"(</svg>)"};

    auto component = componentFor(markup, 200.f, 100.f);

    check(component->getFontCount() == 2);

    // Same size again: nothing changes, and nothing should have been rebuilt.
    component->setBounds({0.f, 0.f, 200.f, 100.f});
    check(component->getFontCount() == 2);

    // A different size asks for two different point sizes, and the two it no
    // longer wants have to go.
    component->setBounds({0.f, 0.f, 400.f, 200.f});
    check(component->getFontCount() == 2, "two sizes, not four");
};
