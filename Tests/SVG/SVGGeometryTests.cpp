#include <eacp/SVG/SVG.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

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

    check(
        isNear(SVG::parseTransformMatrix("skewX(45)").apply({0.f, 2.f}), 2.f, 2.f));

    check(isNear(SVG::parseTransformMatrix("scale(3)").apply({1.f, 2.f}), 3.f, 6.f));
};

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

auto tTwoMasks = test("SVGComponent/anElementFilledAndStrokedIsTwoMasks") = []
{
    auto both = componentFor(
        R"(<svg width="100" height="100"><rect x="10" y="10" width="50" height="50" fill="red" stroke="blue" stroke-width="2"/></svg>)");

    check(both->getShapeCount() == 2);

    auto fillOnly = componentFor(
        R"(<svg width="100" height="100"><rect x="10" y="10" width="50" height="50" fill="red"/></svg>)");

    check(fillOnly->getShapeCount() == 1);
};

// Nothing here can read a colour off a mask, so inherited fill="none" is
// observed as the absence of a fill mask.
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

auto tAspectDefault = test("PreserveAspectRatio/theDefaultIsUniformAndCentred") = []
{
    auto fit = SVG::parsePreserveAspectRatio("");

    check(fit.uniform);
    check(!fit.slice);
    check(fit.x == SVG::PreserveAspectRatio::Align::Mid);
    check(fit.y == SVG::PreserveAspectRatio::Align::Mid);

    // 200x100 into 100x100: both axes at 0.5, with the 50 spare height split.
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

    // meet takes the smaller scale, slice the larger.
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

auto tStyleDeclarations =
    test("SVGAttributes/styleDeclarationsAreReadAsProperties") = []
{
    auto declarations =
        SVG::parseStyleDeclarations("  fill : #ff0000 ; stroke-width:2.5;  ");

    check(declarations.size() == 2, "the trailing semicolon is not a third");
    check(declarations["fill"] == "#ff0000", "the value is trimmed");
    check(declarations["stroke-width"] == "2.5");
};

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

auto tArcFlagsPickTheArc = test("SVGPathParser/theTwoArcFlagsPickOneOfFourArcs") = []
{
    auto boundsOf = [](const std::string& d)
    { return SVG::parseSVGPath<GPUWidgets::Path>(d).getBounds(); };

    // In SVG's y-down space sweep=1 is clockwise on screen, so the bulge is up.
    auto up = boundsOf("M 0 50 A 50 50 0 0 1 100 50");
    auto down = boundsOf("M 0 50 A 50 50 0 0 0 100 50");

    check(isNear(up.y, 0.f) && isNear(up.h, 50.f));
    check(isNear(down.y, 50.f) && isNear(down.h, 50.f));

    // large-arc on a quarter turn takes the three quarters, so the same
    // endpoints enclose the whole circle's extent.
    auto minor = boundsOf("M 50 0 A 50 50 0 0 1 100 50");
    auto major = boundsOf("M 50 0 A 50 50 0 1 1 100 50");

    check(isNear(minor.w, 50.f) && isNear(minor.h, 50.f));
    check(isNear(major.w, 100.f) && isNear(major.h, 100.f));
};

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

// Minifiers run the single-character flags into what follows: read as numbers,
// "0150" is one value and every coordinate after it lands somewhere else.
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

auto tUseInheritsFromTheUseSite =
    test("SVGComponent/referencedContentInheritsFromTheUseSite") = []
{
    auto component = componentFor(
        R"(<svg width="100" height="100"><defs><rect id="box" x="0" y="0" width="10" height="10"/></defs>)"
        R"(<use href="#box" fill="none"/></svg>)");

    check(component->getShapeCount() == 0, "the use site's fill:none reached it");
};

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

    // The 10x10 symbol drawn at the 50x50 the use asked for is 2500 of mask.
    check(component->getTotalMaskArea() > 2000.f
              && component->getTotalMaskArea() < 3000.f,
          "the symbol's viewBox was mapped onto the use's size");
};

// What the dashes actually are is pinned in PathStrokerTests.
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

// The point size is the document's times the transform's scale, so a resize
// asks for new sizes and a cache that only grew would hold one per drag frame.
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

    component->setBounds({0.f, 0.f, 200.f, 100.f});
    check(component->getFontCount() == 2);

    component->setBounds({0.f, 0.f, 400.f, 200.f});
    check(component->getFontCount() == 2, "two sizes, not four");
};
