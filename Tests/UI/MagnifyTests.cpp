#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace MagnifyTests
{
namespace
{
struct Pinchable final : Component
{
    explicit Pinchable(bool consumesToUse = true)
        : consumes(consumesToUse)
    {
        setInterceptsMouseClicks(true);
    }

    bool mouseMagnify(const MouseEvent& event) override
    {
        ++magnifies;
        last = event;
        return consumes;
    }

    bool consumes = true;
    int magnifies = 0;
    MouseEvent last;
};

eacp::Graphics::MouseEvent pinchAt(Point position, float scale)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.type = eacp::Graphics::MouseEventType::Magnify;
    event.pos = position;
    event.downPos = position;
    event.magnifyScale = scale;
    event.scrollPhase = eacp::Graphics::ScrollPhase::Changed;

    return event;
}

struct Tree
{
    Tree()
    {
        host.setBounds({0.f, 0.f, 400.f, 300.f});
        host.setRootComponent(root);

        root.addAndMakeVisible(parent);
        parent.setBounds({100.f, 50.f, 200.f, 200.f});

        parent.addAndMakeVisible(child);
        child.setBounds({20.f, 30.f, 50.f, 50.f});
    }

    ComponentHost host;
    Component root;
    Pinchable parent;
    Pinchable child;
};
} // namespace

auto tReachesHovered = test("Magnify/reachesTheComponentUnderThePointer") = []
{
    auto tree = Tree {};

    tree.host.mouseMagnify(pinchAt({130.f, 90.f}, 1.25f));

    check(tree.child.magnifies == 1);
    check(tree.parent.magnifies == 0, "consumed by the child");
    check(tree.child.last.magnifyScale == 1.25f);
    check(tree.child.last.position.x == 10.f);
    check(tree.child.last.position.y == 10.f, "in the child's own space");
};

auto tBubbles = test("Magnify/anUnconsumedPinchGoesUpToTheParent") = []
{
    auto tree = Tree {};
    tree.child.consumes = false;

    auto event = pinchAt({130.f, 90.f}, 0.8f);
    event.modifiers.command = true;
    tree.host.mouseMagnify(event);

    check(tree.child.magnifies == 1);
    check(tree.parent.magnifies == 1);
    check(tree.parent.last.magnifyScale == 0.8f);
    check(tree.parent.last.modifiers.command);
    check(tree.parent.last.position.x == 30.f);
    check(tree.parent.last.position.y == 40.f, "in the parent's own space");
};

auto tNothingUnder = test("Magnify/aPinchOverNothingGoesNowhere") = []
{
    auto tree = Tree {};

    tree.host.mouseMagnify(pinchAt({20.f, 20.f}, 1.5f));

    check(tree.child.magnifies == 0);
    check(tree.parent.magnifies == 0);
};

auto tNoRoot = test("Magnify/aHostWithoutARootIgnoresThePinch") = []
{
    auto host = ComponentHost {};
    host.setBounds({0.f, 0.f, 100.f, 100.f});

    host.mouseMagnify(pinchAt({50.f, 50.f}, 1.5f));
};

auto tIgnoresPress = test("Magnify/goesToThePointerNotToThePressedComponent") = []
{
    auto tree = Tree {};
    auto other = Pinchable {};
    tree.root.addAndMakeVisible(other);
    other.setBounds({0.f, 0.f, 50.f, 50.f});

    auto down = pinchAt({10.f, 10.f}, 1.f);
    down.type = eacp::Graphics::MouseEventType::Down;
    tree.host.mouseDown(down);

    tree.host.mouseMagnify(pinchAt({130.f, 90.f}, 1.1f));

    check(other.magnifies == 0);
    check(tree.child.magnifies == 1);
};

auto tWheelIsNotMagnify = test("Magnify/theWheelDoesNotReachMouseMagnify") = []
{
    auto tree = Tree {};

    auto wheel = pinchAt({130.f, 90.f}, 1.f);
    wheel.type = eacp::Graphics::MouseEventType::Wheel;
    wheel.delta = {0.f, -3.f};
    tree.host.mouseWheel(wheel);

    check(tree.child.magnifies == 0);
    check(tree.parent.magnifies == 0);
};
} // namespace MagnifyTests
