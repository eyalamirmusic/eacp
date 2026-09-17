#include "Common.h"

// Lifetime of the hover/capture pointers a dispatching view keeps between
// events. dispatchMouseEvent records the deepest hit view (hoveredView,
// mouseDownTarget) on the view the platform event arrived at -- the window's
// root -- but those targets can sit arbitrarily deep in the tree. Removing a
// view, or any ancestor of it, must drop those pointers on every view that
// might still dispatch to it: in production the removed subtree is usually
// destroyed next, and a later mouseExited walks straight into freed memory.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
struct EventRecorder final : View
{
    EventRecorder() { setHandlesMouseEvents(true); }

    void mouseEntered(const MouseEvent&) override { ++enters; }
    void mouseExited(const MouseEvent&) override { ++exits; }
    void mouseDragged(const MouseEvent&) override { ++drags; }
    void mouseUp(const MouseEvent&) override { ++ups; }

    int enters = 0;
    int exits = 0;
    int drags = 0;
    int ups = 0;
};

MouseEvent mouseEvent(MouseEventType type, Point position)
{
    auto event = MouseEvent {};
    event.type = type;
    event.pos = position;
    return event;
}

// root -> holder -> leaf, with the pointer hovering the leaf. The leaf is a
// grandchild on purpose: the root records it as hoveredView, but removal goes
// through the holder, which is the case the direct-child bookkeeping misses.
struct HoverTree
{
    HoverTree()
    {
        root.setBounds({0.f, 0.f, 200.f, 200.f});
        holder.setBounds({0.f, 0.f, 200.f, 200.f});
        leaf.setBounds({50.f, 50.f, 100.f, 100.f});

        root.addSubview(holder);
        holder.addSubview(leaf);
    }

    View root;
    View holder;
    EventRecorder leaf;
};
} // namespace

// The crash from the wild: hover a nested view, its subtree gets removed (a
// closed pane, a dismissed popup), then the pointer leaves the window and the
// platform sends Exited. The detached view must not hear about it -- by then
// it is usually destroyed.
auto tRemovedSubtreeExit =
    test("MouseTracking/removedSubtreeReceivesNoExitEvent") = []
{
    auto tree = HoverTree {};

    tree.root.dispatchMouseEvent(mouseEvent(MouseEventType::Moved, {100.f, 100.f}));
    check(tree.leaf.enters == 1);

    tree.root.removeSubview(tree.holder);

    tree.root.dispatchMouseEvent(mouseEvent(MouseEventType::Exited, {-1.f, -1.f}));
    check(tree.leaf.exits == 0);
};

// Same stale pointer, other route: the next Moved lands on a different view,
// and hover tracking sends the farewell Exited to whatever hoveredView still
// points at.
auto tRemovedSubtreeHoverChange =
    test("MouseTracking/removedSubtreeReceivesNoExitOnHoverChange") = []
{
    auto tree = HoverTree {};

    tree.root.dispatchMouseEvent(mouseEvent(MouseEventType::Moved, {100.f, 100.f}));
    check(tree.leaf.enters == 1);

    tree.root.removeSubview(tree.holder);

    tree.root.dispatchMouseEvent(mouseEvent(MouseEventType::Moved, {10.f, 10.f}));
    check(tree.leaf.exits == 0);
};

// mouseDownTarget has the same lifetime: a drag captured by a view that is
// then torn out of the tree must not keep feeding it Dragged/Up events.
auto tRemovedSubtreeDrag =
    test("MouseTracking/removedSubtreeReceivesNoDragOrUpEvents") = []
{
    auto tree = HoverTree {};

    tree.root.dispatchMouseEvent(mouseEvent(MouseEventType::Down, {100.f, 100.f}));

    tree.root.removeSubview(tree.holder);

    tree.root.dispatchMouseEvent(
        mouseEvent(MouseEventType::Dragged, {110.f, 110.f}));
    tree.root.dispatchMouseEvent(mouseEvent(MouseEventType::Up, {110.f, 110.f}));

    check(tree.leaf.drags == 0);
    check(tree.leaf.ups == 0);
};

// The direct-child case is already handled today; pinned here so the fix for
// the nested case keeps it working.
auto tRemovedDirectChildExit =
    test("MouseTracking/removedDirectChildReceivesNoExitEvent") = []
{
    auto root = View {};
    auto leaf = EventRecorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    leaf.setBounds({50.f, 50.f, 100.f, 100.f});
    root.addSubview(leaf);

    root.dispatchMouseEvent(mouseEvent(MouseEventType::Moved, {100.f, 100.f}));
    check(leaf.enters == 1);

    root.removeSubview(leaf);

    root.dispatchMouseEvent(mouseEvent(MouseEventType::Exited, {-1.f, -1.f}));
    check(leaf.exits == 0);
};
