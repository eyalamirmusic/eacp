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

    // Front to back, the same order a click is resolved in.
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
    // Deliberately not getComponentAt: taking a drop is independent of taking a
    // click, and a component with no drop target is transparent here.
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

    // Exit before the drop, so a highlight has one place to be turned off.
    setTarget(nullptr, positionInRoot);

    auto info = current;

    // Clean up first: itemDropped is free to destroy what was being dragged.
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

    current.source = nullptr;
    cancelDrag();
}
} // namespace eacp::UI
