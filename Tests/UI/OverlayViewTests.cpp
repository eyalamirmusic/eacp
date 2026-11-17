#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// A view laid over a ComponentHost that only draws -- a GPUView rendering a
// scene the host's own tree takes the input for. Routing goes through
// View::dispatchMouseEvent from the root, as every platform's native view
// delivers it, so these say where a press lands and that the overlay never
// hears of it once it ignores the pointer.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace OverlayViewTests
{
namespace
{
struct Pressable final : Component
{
    Pressable() { setInterceptsMouseClicks(true); }

    void mouseDown(const MouseEvent&) override { ++presses; }

    int presses = 0;
};

struct Overlay final : GPU::GPUView
{
    Overlay() { setHandlesMouseEvents(true); }

    void mouseDown(const eacp::Graphics::MouseEvent&) override { ++presses; }

    int presses = 0;
};

struct Tree
{
    Tree()
    {
        host.setBounds({0.f, 0.f, 400.f, 300.f});
        host.setRootComponent(root);

        root.addAndMakeVisible(target);
        target.setBounds({100.f, 50.f, 200.f, 200.f});

        host.addSubview(overlay);
        overlay.setBounds({100.f, 50.f, 200.f, 200.f});
    }

    void pressAt(Point position)
    {
        auto event = eacp::Graphics::MouseEvent {};
        event.pos = position;
        event.downPos = position;
        event.type = eacp::Graphics::MouseEventType::Down;

        host.dispatchMouseEvent(event);
    }

    ComponentHost host;
    Component root;
    Pressable target;
    Overlay overlay;
};

struct TouchCountingView final : eacp::Graphics::View
{
    TouchCountingView() { setHandlesTouchEvents(true); }

    void touchBegan(const eacp::Graphics::TouchEvent&) override { ++touches; }

    int touches = 0;
};
} // namespace

auto tOverlayTakesPressesByDefault =
    test("OverlayView/anOverlayHandlingTheMouseTakesThePress") = []
{
    auto tree = Tree {};

    check(tree.host.hitTest({150.f, 100.f}) == &tree.overlay);

    tree.pressAt({150.f, 100.f});

    check(tree.overlay.presses == 1);
    check(tree.target.presses == 0);
};

auto tIgnoringOverlayLetsPressesThrough =
    test("OverlayView/anOverlayIgnoringTheMouseLetsThePressReachTheTree") = []
{
    auto tree = Tree {};
    tree.overlay.setIgnoresMouseEvents();

    check(tree.host.hitTest({150.f, 100.f}) == &tree.host);

    tree.pressAt({150.f, 100.f});

    check(tree.overlay.presses == 0);
    check(tree.target.presses == 1);
};

auto tIgnoringCoversSubviews =
    test("OverlayView/ignoringTheMouseCoversTheOverlaysSubviews") = []
{
    auto tree = Tree {};
    auto inner = Overlay {};

    tree.overlay.addSubview(inner);
    inner.setBounds({0.f, 0.f, 200.f, 200.f});
    tree.overlay.setIgnoresMouseEvents();

    check(tree.host.hitTest({150.f, 100.f}) == &tree.host);

    tree.overlay.setIgnoresMouseEvents(false);

    check(tree.host.hitTest({150.f, 100.f}) == &inner);

    tree.overlay.removeSubview(inner);
};

auto tHiddenOverlayLetsPressesThrough =
    test("OverlayView/aHiddenOverlayLetsThePressReachTheTree") = []
{
    auto tree = Tree {};
    tree.overlay.setVisible(false);

    check(tree.host.hitTest({150.f, 100.f}) == &tree.host);

    tree.pressAt({150.f, 100.f});

    check(tree.overlay.presses == 0);
    check(tree.target.presses == 1);
};

auto tHiddenViewIsNoTouchTarget = test("OverlayView/aHiddenViewTakesNoTouch") = []
{
    const auto wholeRoot = eacp::Graphics::Rect {0.f, 0.f, 400.f, 300.f};
    const auto touchPosition = Point {150.f, 100.f};

    auto root = eacp::Graphics::View {};
    auto beneath = TouchCountingView {};
    auto hiddenAbove = TouchCountingView {};
    root.setBounds(wholeRoot);
    root.addSubview(beneath);
    root.addSubview(hiddenAbove);
    beneath.setBounds(wholeRoot);
    hiddenAbove.setBounds(wholeRoot);
    hiddenAbove.setVisible(false);

    auto touch = eacp::Graphics::TouchEvent {};
    touch.pos = touchPosition;
    touch.phase = eacp::Graphics::TouchPhase::Began;
    root.dispatchTouchEvent(touch);

    check(beneath.touches == 1);
    check(hiddenAbove.touches == 0);
};

} // namespace OverlayViewTests
