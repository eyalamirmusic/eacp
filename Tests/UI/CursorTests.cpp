#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// Which cursor a component tree hands its one native view: the hovered
// component's, and again whenever that component changes it, with or without
// the pointer moving.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

using Cursor = eacp::Graphics::MouseCursor;

namespace
{
struct Tree
{
    Tree()
    {
        host.setBounds({0.f, 0.f, 200.f, 100.f});
        host.setRootComponent(root);

        root.addAndMakeVisible(left);
        root.addAndMakeVisible(right);

        left.setBounds({0.f, 0.f, 100.f, 100.f});
        right.setBounds({100.f, 0.f, 100.f, 100.f});

        left.setInterceptsMouseClicks(true);
        right.setInterceptsMouseClicks(true);
    }

    void moveTo(Point position)
    {
        auto event = eacp::Graphics::MouseEvent {};
        event.type = eacp::Graphics::MouseEventType::Moved;
        event.pos = position;
        host.mouseMoved(event);
    }

    ComponentHost host;
    Component root;
    Component left;
    Component right;
};

Cursor brush()
{
    auto image = eacp::Graphics::Image {32, 32};
    image.set(5, 5, Color {0.f, 0.f, 0.f, 1.f});

    return {image, {5.f, 5.f}, 1.f};
}
} // namespace

auto tHoveredCursorReachesTheView =
    test("Cursor/theHoveredComponentsCursorReachesTheView") = []
{
    auto tree = Tree {};
    tree.right.setMouseCursor(Cursor::PointingHand);

    tree.moveTo({150.f, 50.f});
    check(tree.host.getMouseCursor() == Cursor::PointingHand);

    tree.moveTo({50.f, 50.f});
    check(tree.host.getMouseCursor() == Cursor::Default);
};

auto tChangedCursorShowsWithoutAMove =
    test("Cursor/aHoveredComponentChangingItsCursorShowsAtOnce") = []
{
    auto tree = Tree {};
    tree.moveTo({50.f, 50.f});

    const auto cursor = brush();
    tree.left.setMouseCursor(cursor);

    check(tree.host.getMouseCursor() == cursor);
    check(tree.host.getMouseCursor().isImage());

    tree.left.setMouseCursor(Cursor::Crosshair);
    check(tree.host.getMouseCursor() == Cursor::Crosshair);
};

auto tOtherComponentsLeaveTheCursorAlone =
    test("Cursor/aComponentNotUnderThePointerLeavesTheCursorAlone") = []
{
    auto tree = Tree {};
    tree.left.setMouseCursor(Cursor::IBeam);
    tree.moveTo({50.f, 50.f});

    tree.right.setMouseCursor(brush());

    check(tree.host.getMouseCursor() == Cursor::IBeam);
};

auto tLeavingRestoresTheArrow = test("Cursor/leavingTheTreeRestoresTheArrow") = []
{
    auto tree = Tree {};
    tree.left.setMouseCursor(brush());
    tree.moveTo({50.f, 50.f});

    check(tree.host.getMouseCursor().isImage());

    tree.host.mouseExited({});
    check(tree.host.getMouseCursor() == Cursor::Default);
};

auto tUpdateMouseCursorReapplies =
    test("Cursor/updateMouseCursorReappliesTheHoveredCursor") = []
{
    auto tree = Tree {};
    tree.left.setMouseCursor(Cursor::ResizeUpDown);
    tree.moveTo({50.f, 50.f});

    tree.host.setMouseCursor(Cursor::Default);
    tree.host.updateMouseCursor();

    check(tree.host.getMouseCursor() == Cursor::ResizeUpDown);
};
