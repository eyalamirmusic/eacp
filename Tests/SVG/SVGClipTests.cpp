#include <eacp/SVG/SVG.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

using namespace nano;
using namespace eacp;

namespace
{
constexpr auto clipTolerance = 0.001f;

bool isNearClip(float a, float b)
{
    return std::abs(a - b) < clipTolerance;
}

bool isNearRect(const Graphics::Rect& rect, float x, float y, float w, float h)
{
    return isNearClip(rect.x, x) && isNearClip(rect.y, y) && isNearClip(rect.w, w)
           && isNearClip(rect.h, h);
}

SVG::SVGElement clipDocumentFrom(const std::string& markup)
{
    auto root = SVG::parseXML(markup);

    return root.has_value() ? *root : SVG::SVGElement {};
}

void collectClipIds(const SVG::SVGElement& element, SVG::ElementsById& byId)
{
    auto id = element.attr("id");

    if (!id.empty())
        byId.emplace(id, &element);

    for (const auto& child: element.children)
        collectClipIds(child, byId);
}

// Nothing here depends on the segment count, only on where the points are.
constexpr auto clipFlatness = 0.05f;

const auto anyBox = Graphics::Rect {0.f, 0.f, 10.f, 10.f};

OwningPointer<SVG::SVGComponent> clippedComponent(const std::string& markup,
                                                  float width = 100.f,
                                                  float height = 100.f)
{
    auto component = makeOwned<SVG::SVGComponent>();

    component->setBounds({0.f, 0.f, width, height});
    component->setDocument(clipDocumentFrom(markup));

    return component;
}
} // namespace

auto tClipReference = test("SVGClip/aClipPathNamesARegionOrIsNotThere") = []
{
    auto document = clipDocumentFrom(
        R"SVG(<svg><defs><clipPath id="c"><rect x="10" y="20" width="30" height="40"/></clipPath></defs></svg>)SVG");

    auto byId = SVG::ElementsById {};
    collectClipIds(document, byId);

    auto region = SVG::resolveClipPath("c", byId, anyBox, clipFlatness);

    check(region.resolved);
    check(!region.isEmpty());
    check(isNearRect(region.path.getBounds(), 10.f, 20.f, 30.f, 40.f));

    auto missing = SVG::resolveClipPath("nothing", byId, anyBox, clipFlatness);

    check(!missing.resolved);
    check(missing.isEmpty());
};

auto tEmptyClipPath = test("SVGClip/aClipPathHoldingNothingIsStillAClipPath") = []
{
    auto document = clipDocumentFrom(
        R"SVG(<svg><defs><clipPath id="c"></clipPath></defs></svg>)SVG");

    auto byId = SVG::ElementsById {};
    collectClipIds(document, byId);

    auto region = SVG::resolveClipPath("c", byId, anyBox, clipFlatness);

    check(region.resolved, "the reference found a clipPath");
    check(region.isEmpty(), "which holds no region at all");
};

auto tClipUnion = test("SVGClip/severalShapesInAClipPathAreTheirUnion") = []
{
    auto document = clipDocumentFrom(
        R"SVG(<svg><defs><clipPath id="c">
             <rect x="0" y="0" width="20" height="20"/>
             <rect x="40" y="30" width="20" height="20"/>
           </clipPath></defs></svg>)SVG");

    auto byId = SVG::ElementsById {};
    collectClipIds(document, byId);

    auto region = SVG::resolveClipPath("c", byId, anyBox, clipFlatness);

    check(region.path.getSubPaths().size() == 2, "both shapes are in the region");
    check(isNearRect(region.path.getBounds(), 0.f, 0.f, 60.f, 50.f),
          "which spans both of them");

    // Under even-odd every overlap between the two would read as a hole.
    check(region.rule == GPUWidgets::FillRule::NonZero);
};

auto tClipChildTransform =
    test("SVGClip/aChildOfAClipPathCarriesItsOwnTransform") = []
{
    auto document = clipDocumentFrom(
        R"SVG(<svg><defs><clipPath id="c">
             <rect x="0" y="0" width="10" height="10" transform="translate(30 40)"/>
           </clipPath></defs></svg>)SVG");

    auto byId = SVG::ElementsById {};
    collectClipIds(document, byId);

    auto region = SVG::resolveClipPath("c", byId, anyBox, clipFlatness);

    check(isNearRect(region.path.getBounds(), 30.f, 40.f, 10.f, 10.f));
};

auto tClipUse = test("SVGClip/aUseInsideAClipPathIsTheShapeItNames") = []
{
    auto document = clipDocumentFrom(
        R"SVG(<svg><defs>
             <rect id="tile" x="0" y="0" width="10" height="10"/>
             <clipPath id="c"><use href="#tile" x="5" y="7"/></clipPath>
           </defs></svg>)SVG");

    auto byId = SVG::ElementsById {};
    collectClipIds(document, byId);

    auto region = SVG::resolveClipPath("c", byId, anyBox, clipFlatness);

    check(!region.isEmpty());
    check(isNearRect(region.path.getBounds(), 5.f, 7.f, 10.f, 10.f),
          "x and y translate what the use names");
};

auto tClipUnits = test("SVGClip/objectBoundingBoxIsFractionsOfTheClippedShape") = []
{
    auto document = clipDocumentFrom(
        R"SVG(<svg><defs>
             <clipPath id="half" clipPathUnits="objectBoundingBox">
               <rect x="0" y="0" width="0.5" height="1"/>
             </clipPath>
             <clipPath id="user"><rect x="0" y="0" width="0.5" height="1"/></clipPath>
           </defs></svg>)SVG");

    auto byId = SVG::ElementsById {};
    collectClipIds(document, byId);

    check(SVG::clipUsesBoundingBox("half", byId));
    check(!SVG::clipUsesBoundingBox("user", byId));

    auto box = Graphics::Rect {100.f, 200.f, 40.f, 60.f};
    auto region = SVG::resolveClipPath("half", byId, box, clipFlatness);

    check(isNearRect(region.path.getBounds(), 100.f, 200.f, 20.f, 60.f),
          "half the box, placed on the box");

    auto plain = SVG::resolveClipPath("user", byId, box, clipFlatness);

    check(isNearRect(plain.path.getBounds(), 0.f, 0.f, 0.5f, 1.f));
};

auto tClipRule = test("SVGClip/clipRuleIsTheRegionsFillRule") = []
{
    auto document = clipDocumentFrom(
        R"SVG(<svg><defs>
             <clipPath id="frame">
               <path d="M 0 0 h 100 v 80 h -100 z M 20 20 h 60 v 40 h -60 z" clip-rule="evenodd"/>
             </clipPath>
             <clipPath id="onTheContainer" clip-rule="evenodd">
               <path d="M 0 0 h 100 v 80 h -100 z M 20 20 h 60 v 40 h -60 z"/>
             </clipPath>
             <clipPath id="unsaid">
               <path d="M 0 0 h 100 v 80 h -100 z M 20 20 h 60 v 40 h -60 z"/>
             </clipPath>
           </defs></svg>)SVG");

    auto byId = SVG::ElementsById {};
    collectClipIds(document, byId);

    check(SVG::resolveClipPath("frame", byId, anyBox, clipFlatness).rule
          == GPUWidgets::FillRule::EvenOdd);

    // A clipPath's own clip-rule is inherited by its children in the CSS sense.
    check(SVG::resolveClipPath("onTheContainer", byId, anyBox, clipFlatness).rule
          == GPUWidgets::FillRule::EvenOdd);

    check(SVG::resolveClipPath("unsaid", byId, anyBox, clipFlatness).rule
          == GPUWidgets::FillRule::NonZero);
};

auto tRectangleClip = test("SVGClip/aRectangularRegionIsRecognisedAsOne") = []
{
    auto rectangle = GPUWidgets::Path {};
    rectangle.addRect({10.f, 20.f, 30.f, 40.f});

    auto found = SVG::asAxisAlignedRect(rectangle);

    check(found.has_value());
    check(isNearRect(*found, 10.f, 20.f, 30.f, 40.f));

    // Closed by repeating the first point rather than by saying so.
    auto written = GPUWidgets::Path {};
    written.moveTo({0.f, 0.f});
    written.lineTo({10.f, 0.f});
    written.lineTo({10.f, 5.f});
    written.lineTo({0.f, 5.f});
    written.lineTo({0.f, 0.f});

    check(SVG::asAxisAlignedRect(written).has_value());

    auto rotated =
        rectangle.transformed(GPUWidgets::AffineTransform::rotation(0.3f));

    check(!SVG::asAxisAlignedRect(rotated).has_value(), "a rotated rectangle");

    auto ellipse = GPUWidgets::Path {};
    ellipse.addEllipse({0.f, 0.f, 10.f, 10.f});

    check(!SVG::asAxisAlignedRect(ellipse).has_value());

    auto two = GPUWidgets::Path {};
    two.addRect({0.f, 0.f, 10.f, 10.f});
    two.addRect({20.f, 0.f, 10.f, 10.f});

    check(!SVG::asAxisAlignedRect(two).has_value(), "two rectangles are not one");

    auto triangle = GPUWidgets::Path {};
    triangle.moveTo({0.f, 0.f});
    triangle.lineTo({10.f, 0.f});
    triangle.lineTo({5.f, 10.f});
    triangle.close();

    check(!SVG::asAxisAlignedRect(triangle).has_value());
};

auto tSharedClip = test("SVGComponent/oneClipRegionServesAWholeClippedGroup") = []
{
    auto component = clippedComponent(
        R"SVG(<svg width="100" height="100">
             <defs><clipPath id="c"><circle cx="50" cy="50" r="30"/></clipPath></defs>
             <g clip-path="url(#c)">
               <rect x="0" y="0" width="40" height="40"/>
               <rect x="20" y="20" width="40" height="40"/>
               <rect x="40" y="40" width="40" height="40"/>
             </g>
           </svg>)SVG");

    check(component->getShapeCount() == 3);
    check(component->getClipCount() == 1, "three shapes, one region");
    check(component->getClipMaskCount() == 1, "a circle is a mask");
};

auto tRectangleCostsNoMask = test("SVGComponent/aRectangularClipTakesNoMask") = []
{
    auto component = clippedComponent(
        R"SVG(<svg width="100" height="100">
             <defs><clipPath id="c"><rect x="10" y="10" width="50" height="50"/></clipPath></defs>
             <circle cx="50" cy="50" r="40" clip-path="url(#c)"/>
           </svg>)SVG");

    check(component->getClipCount() == 1);
    check(component->getClipMaskCount() == 0, "a scissor rect, not a mask");
};

auto tPerElementUnits =
    test("SVGComponent/aBoundingBoxClipIsPlacedAgainstEachElement") = []
{
    auto component = clippedComponent(
        R"SVG(<svg width="100" height="100">
             <defs><clipPath id="c" clipPathUnits="objectBoundingBox">
               <circle cx="0.5" cy="0.5" r="0.4"/>
             </clipPath></defs>
             <rect x="0" y="0" width="40" height="20" clip-path="url(#c)"/>
             <rect x="50" y="50" width="20" height="40" clip-path="url(#c)"/>
           </svg>)SVG");

    check(component->getClipCount() == 2, "one definition, two regions");
};

auto tEmptyClipDrawsNothing =
    test("SVGComponent/anEmptyClipRegionRemovesTheShape") = []
{
    auto clipped = clippedComponent(
        R"SVG(<svg width="100" height="100">
             <defs><clipPath id="c"></clipPath></defs>
             <rect x="10" y="10" width="50" height="50" clip-path="url(#c)"/>
           </svg>)SVG");

    check(clipped->getShapeCount() == 0);

    // A reference naming nothing is ignored, so the element draws unclipped.
    auto unclipped = clippedComponent(
        R"SVG(<svg width="100" height="100">
             <rect x="10" y="10" width="50" height="50" clip-path="url(#missing)"/>
           </svg>)SVG");

    check(unclipped->getShapeCount() == 1);
    check(unclipped->getClipCount() == 0);
};

// One mask reaches a shape, so the inner region takes it and the outer
// contributes its rectangle -- exact only while the outer clip is rectangular.
auto tNestedClips = test("SVGComponent/aClipInsideAClipKeepsBoth") = []
{
    auto component = clippedComponent(
        R"SVG(<svg width="100" height="100">
             <defs>
               <clipPath id="band"><rect x="0" y="0" width="100" height="50"/></clipPath>
               <clipPath id="lens"><circle cx="50" cy="50" r="30"/></clipPath>
             </defs>
             <g clip-path="url(#band)">
               <rect x="0" y="0" width="100" height="100" clip-path="url(#lens)"/>
             </g>
           </svg>)SVG");

    check(component->getShapeCount() == 1);
    check(component->getClipCount() == 2, "both regions were resolved");
    check(component->getClipMaskCount() == 1, "and only the circle took a mask");
};
