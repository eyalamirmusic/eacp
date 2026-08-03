#include "DragAndDrop.h"

#include <ranges>

namespace eacp::UI
{
DragAndDropTarget::DragAndDropTarget(Component& ownerToUse)
    : owner(ownerToUse)
{
    owner.setDropTarget(this);
}

DragAndDropTarget::~DragAndDropTarget()
{
    owner.setDropTarget(nullptr);
}

DragAndDropContainer::DragAndDropContainer(Component& ownerToUse)
    : owner(ownerToUse)
{
    owner.setDragContainer(this);
}

DragAndDropContainer::~DragAndDropContainer()
{
    owner.setDragContainer(nullptr);
}

void DragAndDropContainer::startDragging(
    DragInfo info,
    Component& source,
    Point imageSizeToUse,
    std::function<void(Graphics&, const Rect&)> paintImage)
{
    if (dragging)
        finishDrag();

    current = std::move(info);
    current.source = &source;

    imageSize = imageSizeToUse;

    image.painter = std::move(paintImage);
    image.setBounds({0.f, 0.f, imageSize.x, imageSize.y});

    owner.addAndMakeVisible(image);

    // Last among the owner's children, so it is painted after the columns it is
    // being dragged over rather than under them.
    image.toFront();

    dragging = true;

    moveImageTo(source.localPointToRoot({}));
}

void DragAndDropContainer::moveImageTo(Point positionInRoot)
{
    auto inOwner = owner.rootPointToLocal(positionInRoot);

    image.setBounds({inOwner.x - imageSize.x * 0.5f,
                     inOwner.y - imageSize.y * 0.5f,
                     imageSize.x,
                     imageSize.y});
}

DragAndDropTarget* DragAndDropContainer::findTargetIn(Component& component,
                                                      Point localPoint) const
{
    if (!component.isVisible() || !component.getLocalBounds().contains(localPoint))
        return nullptr;

    // Front to back, so the topmost thing under the pointer is asked first --
    // the same order a click is resolved in, and for the same reason.
    for (auto* child: std::ranges::reverse_view(component.getChildren()))
    {
        auto childBounds = child->getBounds();

        if (auto* found = findTargetIn(
                *child,
                {localPoint.x - childBounds.x, localPoint.y - childBounds.y}))
            return found;
    }

    auto* target = component.getDropTarget();

    return target != nullptr && target->isInterestedIn(current) ? target : nullptr;
}

DragAndDropTarget* DragAndDropContainer::findTargetAt(Point positionInRoot) const
{
    // Deliberately not getComponentAt, which answers a different question.
    // Taking a drop and taking a *click* are separate: a column that holds
    // cards without wanting clicks of its own still wants what is dropped on
    // it, and the image being dragged sits over everything while wanting
    // neither. A component with no target of its own is transparent here, so
    // the search falls through it to whatever is behind.
    return findTargetIn(owner, owner.rootPointToLocal(positionInRoot));
}

void DragAndDropContainer::setTarget(DragAndDropTarget* target, Point positionInRoot)
{
    if (currentTarget == target)
        return;

    if (currentTarget != nullptr)
    {
        auto info = current;
        info.position =
            currentTarget->getComponent().rootPointToLocal(positionInRoot);
        currentTarget->itemDragExit(info);
    }

    currentTarget = target;

    if (currentTarget != nullptr)
    {
        auto info = current;
        info.position =
            currentTarget->getComponent().rootPointToLocal(positionInRoot);
        currentTarget->itemDragEnter(info);
    }
}

void DragAndDropContainer::dragTo(Point positionInRoot)
{
    if (!dragging)
        return;

    moveImageTo(positionInRoot);
    setTarget(findTargetAt(positionInRoot), positionInRoot);

    if (currentTarget == nullptr)
        return;

    auto info = current;
    info.position = currentTarget->getComponent().rootPointToLocal(positionInRoot);
    currentTarget->itemDragMove(info);
}

void DragAndDropContainer::drop(Point positionInRoot)
{
    if (!dragging)
        return;

    auto* target = findTargetAt(positionInRoot);

    // Exited before the drop, so a target that highlights itself has one place
    // to turn it off rather than two.
    setTarget(nullptr, positionInRoot);

    auto info = current;

    // The image goes first: the drop is free to destroy the thing that was
    // being dragged, and clearing up afterwards would be clearing up around
    // whatever it did.
    finishDrag();

    if (target != nullptr)
    {
        info.position = target->getComponent().rootPointToLocal(positionInRoot);
        target->itemDropped(info);
    }
}

void DragAndDropContainer::cancelDrag()
{
    if (!dragging)
        return;

    setTarget(nullptr, {});
    finishDrag();
}

void DragAndDropContainer::finishDrag()
{
    owner.removeChildComponent(image);

    dragging = false;
    currentTarget = nullptr;
    current = {};
}

void DragAndDropContainer::componentDeleted(Component& component)
{
    if (currentTarget != nullptr && &currentTarget->getComponent() == &component)
        currentTarget = nullptr;

    if (current.source != &component)
        return;

    // The thing being dragged has gone. Nothing can be dropped, and the info
    // still holds a pointer to it, so the drag ends here rather than carrying on
    // with a dangling source.
    current.source = nullptr;
    cancelDrag();
}
} // namespace eacp::UI
