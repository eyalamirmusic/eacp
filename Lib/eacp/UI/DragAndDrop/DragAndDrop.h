#pragma once

#include "../Component/Component.h"

#include <string>

namespace eacp::UI
{
// What a drag carries: an id rather than a payload pointer, because a drop
// routinely destroys and rebuilds the item it moves.
struct DragInfo
{
    // What kind of thing this is, in terms both ends agree on.
    std::string type;

    int itemId = 0;

    // Null once the source component has gone, the normal end of a move.
    Component* source = nullptr;

    // In the coordinates of whatever is being told about it.
    Point position;
};

// Somewhere a drag can be dropped. Held by a component as a member, and must
// outlive it - registration is what makes it findable from a pointer position.
class DragAndDropTarget
{
public:
    explicit DragAndDropTarget(Component& ownerToUse);
    ~DragAndDropTarget();

    DragAndDropTarget(const DragAndDropTarget&) = delete;
    DragAndDropTarget& operator=(const DragAndDropTarget&) = delete;

    // Refusing sends the search on up the tree. Defaults to accepting.
    std::function<bool(const DragInfo&)> isInterestedIn = [](const DragInfo&)
    { return true; };

    // Enter and exit are matched: a drag exits its target before the drop.
    std::function<void(const DragInfo&)> itemDragEnter = [](const DragInfo&) {};
    std::function<void(const DragInfo&)> itemDragMove = [](const DragInfo&) {};
    std::function<void(const DragInfo&)> itemDragExit = [](const DragInfo&) {};

    std::function<void(const DragInfo&)> itemDropped = [](const DragInfo&) {};

    Component& getComponent() const { return owner; }

private:
    Component& owner;
};

// The ancestor that runs a drag: it draws what follows the pointer and finds
// what is under it. One per subtree, holding every possible source and target.
class DragAndDropContainer
{
public:
    explicit DragAndDropContainer(Component& ownerToUse);
    ~DragAndDropContainer();

    DragAndDropContainer(const DragAndDropContainer&) = delete;
    DragAndDropContainer& operator=(const DragAndDropContainer&) = delete;

    // `paintImage` draws into a component of `imageSize` centred on the pointer.
    // Starting a drag while another runs finishes the first without a drop.
    void startDragging(DragInfo info,
                       Component& source,
                       Point imageSize,
                       std::function<void(Graphics&, const Rect&)> paintImage);

    // Both take the pointer in the *root's* coordinates, which is what
    // Component::localPointToRoot gives a mouse handler.
    void dragTo(Point positionInRoot);
    void drop(Point positionInRoot);

    // Ends without dropping.
    void cancelDrag();

    bool isDragging() const { return dragging; }
    const DragInfo& getCurrentDrag() const { return current; }

private:
    friend class Component;

    // The image that follows the pointer, intercepting nothing so the target
    // search never finds it.
    struct DragImage final : Component
    {
        DragImage() { setInterceptsMouseClicks(false); }

        void paint(Graphics& g) override { painter(g, getLocalBounds()); }

        std::function<void(Graphics&, const Rect&)> painter = [](Graphics&,
                                                                 const Rect&) {};
    };

    // Cancels a drag that started from `component` and drops it as a target.
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
