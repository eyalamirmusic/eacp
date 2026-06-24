#include <eacp/Graphics/Graphics/Keyboard.h>
#include <eacp/Graphics/View/View.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp::Graphics;

// These pin the invariant that keeps "an agent can drive it" identical to
// "a human can drive it": ALL input — real OS events and the debug
// server's input tools alike — enters a View only through
// dispatchMouseEvent / dispatchKeyEvent. The per-view handlers are
// protected, so nothing can route around the hit-testing /
// handlesMouseEvents gate. (Routing around it is the bug that let the
// agent orbit a view a real mouse couldn't.)

namespace
{

struct ProbeView : View
{
    int downs = 0;
    int drags = 0;
    int ups = 0;
    int wheels = 0;
    int keysDown = 0;

    void mouseDown(const MouseEvent&) override { ++downs; }
    void mouseDragged(const MouseEvent&) override { ++drags; }
    void mouseUp(const MouseEvent&) override { ++ups; }
    void mouseWheel(const MouseEvent&) override { ++wheels; }
    void keyDown(const KeyEvent&) override { ++keysDown; }
};

MouseEvent mouseAt(float x, float y, MouseEventType type)
{
    auto event = MouseEvent {};
    event.pos = {x, y};
    event.downPos = {x, y};
    event.type = type;
    return event;
}

} // namespace

// A view that has NOT opted into mouse events is undrivable through the
// canonical dispatch path — the only path real input and the agent's input
// tools take. So automation can never drive a view a human can't.
auto tDispatchRespectsGate = test("View/dispatchRespectsHandlesMouseEvents") = []
{
    auto probe = ProbeView {};
    probe.setBounds({0, 0, 200, 200});

    // Gate closed (the default): the same dispatch the OS and the agent use
    // delivers nothing.
    probe.dispatchMouseEvent(mouseAt(50, 50, MouseEventType::Down));
    probe.dispatchMouseEvent(mouseAt(70, 60, MouseEventType::Dragged));
    probe.dispatchMouseEvent(mouseAt(70, 60, MouseEventType::Up));
    check(probe.downs == 0);
    check(probe.drags == 0);
    check(probe.ups == 0);

    // Gate open: the identical dispatch now drives it.
    probe.setHandlesMouseEvents(true);
    probe.dispatchMouseEvent(mouseAt(50, 50, MouseEventType::Down));
    probe.dispatchMouseEvent(mouseAt(70, 60, MouseEventType::Dragged));
    probe.dispatchMouseEvent(mouseAt(70, 60, MouseEventType::Up));
    check(probe.downs == 1);
    check(probe.drags == 1);
    check(probe.ups == 1);
};

// The standard drag affordance moves a view by the pointer's motion, driven
// purely by the dispatched events — so a human and an agent (synthetic events
// through the same dispatch) drag it identically. A view that didn't opt in
// stays put on the same gesture.
auto tDraggable = test("View/draggableFollowsPointerMotion") = []
{
    auto view = View {};
    view.setDraggable(true);
    view.setBounds({100, 100, 80, 40});

    // Press inside the view (local coords), then move the pointer by (+30, +20).
    view.dispatchMouseEvent(mouseAt(40, 20, MouseEventType::Down));
    view.dispatchMouseEvent(mouseAt(70, 40, MouseEventType::Dragged));
    view.dispatchMouseEvent(mouseAt(70, 40, MouseEventType::Up));

    check(view.getBounds().x == 130);
    check(view.getBounds().y == 120);

    // Same gesture on a non-draggable view: it doesn't move.
    auto fixed = View {};
    fixed.setHandlesMouseEvents(true);
    fixed.setBounds({10, 10, 80, 40});

    fixed.dispatchMouseEvent(mouseAt(40, 20, MouseEventType::Down));
    fixed.dispatchMouseEvent(mouseAt(70, 40, MouseEventType::Dragged));

    check(fixed.getBounds().x == 10);
    check(fixed.getBounds().y == 10);
};

// Keyboard goes through the single dispatchKeyEvent entry — the same one the
// native layer and the agent's `key` tool call.
auto tKeyDispatch = test("View/dispatchKeyEventReachesHandler") = []
{
    auto probe = ProbeView {};

    auto event = KeyEvent {};
    event.type = KeyEventType::Down;
    event.characters = "x";
    probe.dispatchKeyEvent(event);

    check(probe.keysDown == 1);
};

// The native @id locator: setId/findChildById find a view by name, and
// getWindowBounds walks the parent chain so the debug server's click_view can
// turn an id into a window point — no hand-computed pixel coordinates.
auto tViewIdLocator = test("View/idLocatorAndWindowBounds") = []
{
    auto root = View {};
    root.setBounds({0, 0, 400, 400});

    auto panel = View {};
    panel.setId("panel").setBounds({50, 60, 200, 150});

    auto child = View {};
    child.setId("child").setBounds({10, 20, 80, 40});

    panel.addSubview(child);
    root.addSubview(panel);

    check(root.findChildById("panel") == &panel);
    check(root.findChildById("child") == &child);
    check(root.findChildById("missing") == nullptr);

    // Window bounds = local bounds summed up the parent chain.
    auto bounds = child.getWindowBounds();
    check(bounds.x == 60); // 50 + 10
    check(bounds.y == 80); // 60 + 20
    check(bounds.w == 80);
    check(bounds.h == 40);
};
