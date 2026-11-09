#include "Common.h"

#include <memory>

// A view destroyed before its subviews. Nothing owns a view's subviews - they
// are members of some derived class, or of whatever made them - so the order
// their destructors run in is the owner's member order, and a parent going
// first is ordinary: a class that declares its content view after the panel
// inside it destroys the content first. The subviews it leaves behind must
// not keep pointing at it, or the next thing they do through their parent
// (find their window, remove themselves on their own destruction) reads freed
// memory.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
struct HoverCounter final : View
{
    HoverCounter() { setHandlesMouseEvents(true); }

    void mouseEntered(const MouseEvent&) override { ++enters; }
    void mouseExited(const MouseEvent&) override { ++exits; }

    int enters = 0;
    int exits = 0;
};

MouseEvent pointerEvent(MouseEventType type, Point position)
{
    auto event = MouseEvent {};
    event.type = type;
    event.pos = position;
    return event;
}
} // namespace

auto tOrphanForgetsItsParent = test("ViewLifetime/subviewOutlivesItsParent") = []
{
    auto child = View {};
    auto parent = std::make_unique<View>();

    parent->setBounds({0.f, 0.f, 100.f, 100.f});
    child.setBounds({10.f, 10.f, 20.f, 20.f});
    parent->addSubview(child);

    parent.reset();

    check(child.getParent() == nullptr);
    check(child.getWindow() == nullptr);

    // Still a working view, and one that can be re-parented and then destroyed
    // the ordinary way.
    child.setBounds({0.f, 0.f, 30.f, 30.f});
    child.repaint();

    auto adopter = View {};
    adopter.addSubview(child);
    check(child.getParent() == &adopter);
    check(adopter.getSubviews().size() == 1);
};

// The same with the orphan destroyed rather than reused: its own
// removeFromParent must find no parent to remove itself from.
auto tOrphanDestroyedLater = test("ViewLifetime/orphanIsDestroyedAfterItsParent") = []
{
    auto child = std::make_unique<View>();
    auto parent = std::make_unique<View>();

    parent->addSubview(*child);

    parent.reset();
    child->removeFromParent();
    child.reset();

    check(true);
};

// A parent between the root and the hovered view. The root keeps the hover
// pointer, so the parent's removal from the root has to clear it while the
// parent is still the leaf's parent: were the leaf detached first, the root
// would no longer see the leaf inside what was removed, and would keep
// pointing at it after the leaf, too, is gone.
auto tDestroyedParentClearsRootHover =
    test("ViewLifetime/destroyedParentClearsTheRootsHoverOnItsSubviews") = []
{
    auto root = View {};
    auto leaf = std::make_unique<HoverCounter>();
    auto holder = std::make_unique<View>();

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    holder->setBounds({0.f, 0.f, 200.f, 200.f});
    leaf->setBounds({50.f, 50.f, 100.f, 100.f});

    root.addSubview(*holder);
    holder->addSubview(*leaf);

    root.dispatchMouseEvent(pointerEvent(MouseEventType::Moved, {100.f, 100.f}));
    check(leaf->enters == 1);

    holder.reset();

    root.dispatchMouseEvent(pointerEvent(MouseEventType::Exited, {-1.f, -1.f}));
    check(leaf->exits == 0);

    leaf.reset();

    root.dispatchMouseEvent(pointerEvent(MouseEventType::Moved, {100.f, 100.f}));
    root.dispatchMouseEvent(pointerEvent(MouseEventType::Exited, {-1.f, -1.f}));
};
