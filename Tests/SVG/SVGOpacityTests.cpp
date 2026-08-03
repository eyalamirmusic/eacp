#include <eacp/SVG/SVG.h>

#include <NanoTest/NanoTest.h>

#include <string>

// Which `opacity` a document meant, which is a question about the element it is
// written on rather than about the number.
//
// On a shape it is the colour's alpha and costs nothing. On a container it is
// the *group's*, and the two are the same picture until the group overlaps
// itself -- where multiplying it into each child stacks two half-transparent
// shapes and darkens the join, and compositing the group draws them solid and
// fades the result once. Only the second is what the format says, and it needs a
// texture and a render pass to do.
//
// So what is pinned here is which elements take one. A texture per faded group
// is the cost, and a document that fades a hundred shapes one at a time should
// pay none of it.

using namespace nano;
using namespace eacp;

namespace
{
OwningPointer<SVG::SVGComponent> opacityComponent(const std::string& markup)
{
    auto root = SVG::parseXML(markup);
    auto component = makeOwned<SVG::SVGComponent>();

    component->setBounds({0.f, 0.f, 100.f, 100.f});

    if (root.has_value())
        component->setDocument(*root);

    return component;
}
} // namespace

auto tShapeOpacityIsItsColour =
    test("SVGComponent/opacityOnAShapeIsItsColourAndNotAGroup") = []
{
    auto component = opacityComponent(
        R"SVG(<svg width="100" height="100">
                <circle cx="50" cy="50" r="20" fill="red" opacity="0.5"/>
              </svg>)SVG");

    check(component->getShapeCount() == 1);
    check(component->getOpacityGroupCount() == 0,
          "a shape composites with nothing, so there is nothing to isolate");
};

auto tGroupOpacityIsALayer =
    test("SVGComponent/opacityOnAContainerCompositesTheGroup") = []
{
    auto component = opacityComponent(
        R"SVG(<svg width="100" height="100">
                <g opacity="0.5">
                  <circle cx="40" cy="50" r="20" fill="red"/>
                  <circle cx="60" cy="50" r="20" fill="red"/>
                </g>
              </svg>)SVG");

    check(component->getShapeCount() == 2);
    check(component->getOpacityGroupCount() == 1, "one group, one texture");
};

// The common case, and the one that must stay free: a container that says
// nothing about its opacity, or says it is opaque, is not a group.
auto tOpaqueGroupCostsNothing =
    test("SVGComponent/anOpaqueContainerTakesNoTexture") = []
{
    auto unsaid = opacityComponent(
        R"SVG(<svg width="100" height="100">
                <g><circle cx="50" cy="50" r="20" fill="red"/></g>
              </svg>)SVG");

    check(unsaid->getOpacityGroupCount() == 0);

    auto stated = opacityComponent(
        R"SVG(<svg width="100" height="100">
                <g opacity="1"><circle cx="50" cy="50" r="20" fill="red"/></g>
              </svg>)SVG");

    check(stated->getOpacityGroupCount() == 0);
};

// A group inside a group is two textures, and the inner one has to be rendered
// before the outer one it is drawn into -- which is why the builder makes them
// innermost-first. Nothing here can watch the order, but the count is what says
// both exist to be ordered.
auto tNestedGroups = test("SVGComponent/aFadedGroupInsideAFadedGroupIsTwo") = []
{
    auto component = opacityComponent(
        R"SVG(<svg width="100" height="100">
                <g opacity="0.7">
                  <rect x="0" y="0" width="60" height="60" fill="green"/>
                  <g opacity="0.6">
                    <circle cx="30" cy="30" r="10" fill="orange"/>
                  </g>
                </g>
              </svg>)SVG");

    check(component->getShapeCount() == 2);
    check(component->getOpacityGroupCount() == 2);
};

// The style attribute spells it too, and beats the presentation attribute of the
// same name as every other property does.
auto tOpacityThroughStyle = test("SVGComponent/opacityIsReadFromStyleToo") = []
{
    auto component = opacityComponent(
        R"SVG(<svg width="100" height="100">
                <g style="opacity:0.4" opacity="1">
                  <circle cx="50" cy="50" r="20" fill="red"/>
                </g>
              </svg>)SVG");

    check(component->getOpacityGroupCount() == 1,
          "the declaration wins, so this group is faded");
};

// A container that drew nothing has nothing to composite, and a texture for it
// would be a render pass over no pixels.
auto tEmptyGroup =
    test("SVGComponent/aFadedContainerWithNoContentTakesNoTexture") = []
{
    auto component = opacityComponent(
        R"SVG(<svg width="100" height="100"><g opacity="0.5"></g></svg>)SVG");

    check(component->getOpacityGroupCount() == 0);
};

// Nothing about a group changes what its children are: the shapes, their clips
// and their gradients are built exactly as they would have been, and only where
// they are drawn to differs.
auto tGroupKeepsItsContent =
    test("SVGComponent/compositingAGroupChangesNothingInsideIt") = []
{
    auto plain = opacityComponent(
        R"SVG(<svg width="100" height="100">
                <g>
                  <rect x="10" y="10" width="30" height="30" fill="red" stroke="blue" stroke-width="2"/>
                  <text x="10" y="80" font-size="10">hi</text>
                </g>
              </svg>)SVG");

    auto faded = opacityComponent(
        R"SVG(<svg width="100" height="100">
                <g opacity="0.5">
                  <rect x="10" y="10" width="30" height="30" fill="red" stroke="blue" stroke-width="2"/>
                  <text x="10" y="80" font-size="10">hi</text>
                </g>
              </svg>)SVG");

    check(faded->getShapeCount() == plain->getShapeCount());
    check(faded->getFontCount() == plain->getFontCount());
    check(faded->getOpacityGroupCount() == 1);
};
