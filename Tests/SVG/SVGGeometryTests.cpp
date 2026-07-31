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
