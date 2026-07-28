#include "Component.h"

#include "../Host/ComponentHost.h"

#include <ranges>

namespace eacp::UI
{
Component::Component() = default;

Component::~Component()
{
    // Before unlinking, while walking up to the host still reaches it: the host
    // may be holding this as its root, its hover or its press capture, and any
    // of those outliving the component is a dangling write later.
    if (auto* found = findHost())
        found->componentDeleted(*this);

    for (auto* child: children)
        child->parent = nullptr;

    if (parent != nullptr)
        parent->removeChildComponent(*this);
}

void Component::setBounds(const Rect& newBounds)
{
    auto sizeChanged = newBounds.w != bounds.w || newBounds.h != bounds.h;

    bounds = newBounds;

    if (sizeChanged)
        resized();

    repaint();
}

void Component::addChildComponent(Component& child)
{
    if (child.parent == this)
        return;

    if (child.parent != nullptr)
        child.parent->removeChildComponent(child);

    child.parent = this;
    children.add(&child);

    repaint();
}

void Component::addAndMakeVisible(Component& child)
{
    addChildComponent(child);
    child.setVisible(true);
}

void Component::removeChildComponent(Component& child)
{
    if (child.parent != this)
        return;

    child.parent = nullptr;
    children.removeAllMatches(&child);

    repaint();
}

void Component::setVisible(bool shouldBeVisible)
{
    if (visible == shouldBeVisible)
        return;

    visible = shouldBeVisible;
    repaint();
}

void Component::toFront()
{
    if (parent == nullptr)
        return;

    parent->children.removeAllMatches(this);
    parent->children.add(this);
    repaint();
}

void Component::toBack()
{
    if (parent == nullptr)
        return;

    parent->children.removeAllMatches(this);
    parent->children.insert(0, this);
    repaint();
}

void Component::setInterceptsMouseClicks(bool shouldIntercept)
{
    interceptsMouseClicks = shouldIntercept;
}

void Component::setMouseCursor(eacp::Graphics::MouseCursor cursorToUse)
{
    cursor = cursorToUse;
}

ComponentHost* Component::findHost() const
{
    auto* current = this;

    while (current->parent != nullptr)
        current = current->parent;

    return current->host;
}

void Component::repaint()
{
    if (auto* found = findHost())
        found->repaint();
}

bool Component::hitTest(Point localPoint) const
{
    return getLocalBounds().contains(localPoint);
}

Component* Component::getComponentAt(Point localPoint)
{
    if (!visible || !hitTest(localPoint))
        return nullptr;

    for (auto* child: std::ranges::reverse_view(children))
    {
        auto childBounds = child->getBounds();
        auto childPoint =
            Point {localPoint.x - childBounds.x, localPoint.y - childBounds.y};

        if (auto* hit = child->getComponentAt(childPoint))
            return hit;
    }

    return interceptsMouseClicks ? this : nullptr;
}

Point Component::localPointToRoot(Point localPoint) const
{
    auto result = localPoint;
    auto* current = this;

    while (current->parent != nullptr)
    {
        result.x += current->bounds.x;
        result.y += current->bounds.y;
        current = current->parent;
    }

    return result;
}

Point Component::rootPointToLocal(Point rootPoint) const
{
    auto offset = localPointToRoot({});
    return {rootPoint.x - offset.x, rootPoint.y - offset.y};
}

int Component::countComponentsInTree() const
{
    auto total = 1;

    for (const auto* child: children)
        total += child->countComponentsInTree();

    return total;
}
} // namespace eacp::UI
