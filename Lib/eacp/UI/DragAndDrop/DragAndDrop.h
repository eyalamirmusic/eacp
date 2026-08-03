#pragma once

#include "../Component/Component.h"

#include <string>

namespace eacp::UI
{
// What a drag carries.
//
// A name and an id rather than a pointer to the thing being dragged, because a
// drop routinely outlives its source: moving a card between two columns means
// destroying the card the drag started from and building another one somewhere
// else, and a payload pointer would be dangling in the middle of the callback
// that does it. The two ends agree on an identifier and look the item up.
struct DragInfo
{
    // What kind of thing this is, in terms both ends agree on. A target that
    // takes cards and not files tests this and says no.
    std::string type;

    int itemId = 0;

    // Where the drag started. Null once that component has gone, which is the
    // normal end of a drag that moves something.
    Component* source = nullptr;

    // The pointer, in the coordinates of whatever is being told about it.
    Point position;
};

// Somewhere a drag can be dropped.
//
// Held by a component as a member, the way a PathShape is, rather than being an
// interface the component inherits: registration is what makes a target
// findable by walking up from whatever the pointer is over, and it is what lets
// the whole feature stay out of Component apart from one pointer.
class DragAndDropTarget
{
public:
    explicit DragAndDropTarget(Component& ownerToUse);
    ~DragAndDropTarget();

    DragAndDropTarget(const DragAndDropTarget&) = delete;
    DragAndDropTarget& operator=(const DragAndDropTarget&) = delete;

    // Whether this target wants this drag at all. Said no, the search carries on
    // up the tree -- so a column that refuses a file still lets the board behind
    // it take one.
    std::function<bool(const DragInfo&)> isInterestedIn = [](const DragInfo&)
    { return true; };

    // Entered, moved within, left. Enter and exit are matched: a drag that ends
    // over a target exits it before the drop, so a target highlighting itself
    // has one place to turn it off.
    std::function<void(const DragInfo&)> itemDragEnter = [](const DragInfo&) {};
    std::function<void(const DragInfo&)> itemDragMove = [](const DragInfo&) {};
    std::function<void(const DragInfo&)> itemDragExit = [](const DragInfo&) {};

    std::function<void(const DragInfo&)> itemDropped = [](const DragInfo&) {};

    Component& getComponent() const { return owner; }

private:
    Component& owner;
};

// The ancestor that runs a drag: it draws what follows the pointer and finds
// what is under it.
//
// One per subtree that drags things about -- normally the component holding
// every possible source and target. The image it draws is its own child and
// brought to the front, which is what puts it above the columns rather than
// clipped inside the one the drag started in.
class DragAndDropContainer
{
public:
    explicit DragAndDropContainer(Component& ownerToUse);
    ~DragAndDropContainer();

    DragAndDropContainer(const DragAndDropContainer&) = delete;
    DragAndDropContainer& operator=(const DragAndDropContainer&) = delete;

    // Begins a drag. `paintImage` draws whatever follows the pointer, into a
    // component of `imageSize` centred on it -- a small version of the thing
    // being dragged, which is cheaper and clearer than a snapshot of it.
    //
    // Starting one while another is running finishes the first without a drop,
    // so a source that forgets to end its drag cannot leave an image behind.
    void startDragging(DragInfo info,
                       Component& source,
                       Point imageSize,
                       std::function<void(Graphics&, const Rect&)> paintImage);

    // Both take the pointer in the *root's* coordinates, which is what
    // Component::localPointToRoot gives a mouse handler.
    void dragTo(Point positionInRoot);
    void drop(Point positionInRoot);

    // Ends without dropping: what Escape means, and what happens when the thing
    // being dragged goes away underneath it.
    void cancelDrag();

    bool isDragging() const { return dragging; }
    const DragInfo& getCurrentDrag() const { return current; }

private:
    friend class Component;

    // The image that follows the pointer. Intercepts nothing, so the search for
    // a target under the pointer never finds it.
    struct DragImage final : Component
    {
        DragImage() { setInterceptsMouseClicks(false); }

        void paint(Graphics& g) override { painter(g, getLocalBounds()); }

        std::function<void(Graphics&, const Rect&)> painter = [](Graphics&,
                                                                 const Rect&) {};
    };

    // A component in this subtree is going away. Cancels a drag that started
    // from it and forgets a target that was holding it.
    void componentDeleted(Component& component);

    DragAndDropTarget* findTargetAt(Point positionInRoot) const;
    DragAndDropTarget* findTargetIn(Component& component, Point localPoint) const;
    void setTarget(DragAndDropTarget* target, Point positionInRoot);
    void moveImageTo(Point positionInRoot);
    void finishDrag();

    Component& owner;
    DragImage image;

    DragInfo current;
    DragAndDropTarget* currentTarget = nullptr;

    Point imageSize {160.f, 48.f};
    bool dragging = false;
};
} // namespace eacp::UI
