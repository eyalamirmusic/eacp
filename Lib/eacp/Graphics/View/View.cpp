#include "View.h"
#include <ranges>

namespace eacp::Graphics
{
View& View::setHandlesMouseEvents(bool value)
{
    properties.handlesMouseEvents = value;
    return *this;
}

View& View::setGrabsFocusOnMouseDown(bool value)
{
    properties.grabsFocusOnMouseDown = value;
    return *this;
}

View& View::setDraggable(bool value)
{
    properties.draggable = value;

    // A view has to be a hit target to be grabbed; dragging without handling
    // mouse events is meaningless.
    if (value)
        properties.handlesMouseEvents = true;

    return *this;
}

View& View::setId(std::string newId)
{
    viewId = std::move(newId);
    return *this;
}

View* View::findChildById(const std::string& target)
{
    if (viewId == target)
        return this;

    for (auto* child: subviews)
        if (auto* found = child->findChildById(target))
            return found;

    return nullptr;
}

Rect View::getWindowBounds() const
{
    auto bounds = getBounds();

    for (auto* ancestor = parent; ancestor != nullptr; ancestor = ancestor->parent)
    {
        auto parentBounds = ancestor->getBounds();
        bounds.x += parentBounds.x;
        bounds.y += parentBounds.y;
    }

    return bounds;
}

void View::removeFromParent()
{
    if (parent != nullptr)
        parent->removeSubview(*this);

    parent = nullptr;
}

void View::resized() {}

Rect View::getLocalBounds() const
{
    auto b = getBounds();
    b.x = 0.f;
    b.y = 0.f;

    return b;
}

void View::addChildren(ChildViews views)
{
    for (auto& view: views)
        addSubview(view);
}

void View::addSubview(View& view)
{
    if (subviews.contains(&view))
        return;

    view.removeFromParent();
    view.parent = this;
    subviews.add(&view);

    viewAdded(view);
}

void View::removeSubview(View& view)
{
    if (subviews.removeAllMatches(&view) > 0)
    {
        if (hoveredView == &view)
            hoveredView = nullptr;

        if (mouseDownTarget == &view)
            mouseDownTarget = nullptr;

        viewRemoved(view);
    }
}

Rect View::getRelativeBounds(const Rect& ratio) const
{
    return getLocalBounds().getRelative(ratio);
}

void View::setBoundsRelative(const Rect& ratio)
{
    if (parent != nullptr)
    {
        setBounds(parent->getRelativeBounds(ratio));
    }
}

void View::scaleToFit()
{
    setBoundsRelative({0.f, 0.f, 1.f, 1.f});
}

void View::scaleToFit(ChildViews views)
{
    for (auto& view: views)
        view.get().scaleToFit();
}

View* View::hitTest(const Point& point)
{
    if (!getLocalBounds().contains(point))
        return nullptr;

    for (auto child: std::ranges::reverse_view(subviews))
    {
        auto childBounds = child->getBounds();
        auto childPoint = Point {point.x - childBounds.x, point.y - childBounds.y};

        if (auto* hit = child->hitTest(childPoint))
            return hit;
    }

    if (properties.handlesMouseEvents)
        return this;

    return nullptr;
}

Point View::convertPointToDescendant(const Point& point, View* descendant) const
{
    if (descendant == nullptr || descendant == this)
        return point;

    auto offset = Point(0.f, 0.f);

    auto current = descendant;

    while (current != nullptr && current != this)
    {
        auto bounds = current->getBounds();
        offset.x += bounds.x;
        offset.y += bounds.y;
        current = current->parent;
    }

    return {point.x - offset.x, point.y - offset.y};
}

Point View::getMousePosition() const
{
    // The root view (where native events enter) tracks the latest dispatched
    // pointer position; convert it into this view's local space. It's the same
    // position the events carried, so a real mouse and an agent's synthetic
    // events report identically — no OS-cursor side channel.
    auto* root = this;
    while (root->parent != nullptr)
        root = root->parent;

    return root->convertPointToDescendant(root->lastPointerPos,
                                          const_cast<View*>(this));
}

MouseEvent View::createLocalEvent(const MouseEvent& event,
                                  View* target,
                                  MouseEventType type)
{
    MouseEvent localEvent = event;
    localEvent.pos = convertPointToDescendant(event.pos, target);
    localEvent.downPos = convertPointToDescendant(event.downPos, target);
    localEvent.type = type;
    return localEvent;
}

void View::forwardDragOrUpToCapturedTarget(const MouseEvent& event)
{
    if (mouseDownTarget != nullptr)
    {
        // Standard drag affordance: move the captured target by the pointer's
        // motion since the last step. Driven entirely by the event position
        // (this view's coordinate space, same as the target's bounds), so it
        // is correct for a real mouse and an agent's synthetic events alike —
        // no OS-cursor side channel. The target's own mouseDragged still runs.
        if (event.type == MouseEventType::Dragged
            && mouseDownTarget->properties.draggable)
        {
            auto bounds = mouseDownTarget->getBounds();
            mouseDownTarget->setBounds(
                bounds.withPosition(bounds.x + (event.pos.x - lastDragPos.x),
                                    bounds.y + (event.pos.y - lastDragPos.y)));
            lastDragPos = event.pos;
        }

        mouseDownTarget->handleMouseEvent(
            createLocalEvent(event, mouseDownTarget, event.type));
    }

    if (event.type == MouseEventType::Up)
        mouseDownTarget = nullptr;
}

void View::updateHoverTracking(View* target, const MouseEvent& event)
{
    if (target == hoveredView)
        return;

    if (hoveredView != nullptr)
    {
        hoveredView->handleMouseEvent(
            createLocalEvent(event, hoveredView, MouseEventType::Exited));
    }

    if (target != nullptr)
    {
        target->handleMouseEvent(
            createLocalEvent(event, target, MouseEventType::Entered));
    }

    hoveredView = target;
}

void View::dispatchHoverEvent(View* target, const MouseEvent& event)
{
    updateHoverTracking(target, event);

    if (target != nullptr && event.type == MouseEventType::Moved)
    {
        target->handleMouseEvent(
            createLocalEvent(event, target, MouseEventType::Moved));
    }
}

void View::dispatchExitEvent(const MouseEvent& event)
{
    if (hoveredView == nullptr)
        return;

    hoveredView->handleMouseEvent(
        createLocalEvent(event, hoveredView, MouseEventType::Exited));
    hoveredView = nullptr;
}

void View::dispatchMouseDown(View* target, const MouseEvent& event)
{
    mouseDownTarget = target;

    if (target != nullptr)
    {
        // Anchor the drag at the press position (see
        // forwardDragOrUpToCapturedTarget).
        if (target->properties.draggable)
            lastDragPos = event.pos;

        target->handleMouseEvent(createLocalEvent(event, target, event.type));
    }
}

void View::dispatchMouseEvent(const MouseEvent& event)
{
    // Record where the pointer is from the event itself, so getMousePosition()
    // reflects this stream (real or synthetic) rather than the OS cursor.
    lastPointerPos = event.pos;

    if (event.type == MouseEventType::Dragged || event.type == MouseEventType::Up)
    {
        forwardDragOrUpToCapturedTarget(event);
        return;
    }

    auto* target = hitTest(event.pos);

    if (event.type == MouseEventType::Moved || event.type == MouseEventType::Entered)
    {
        dispatchHoverEvent(target, event);
        return;
    }

    if (event.type == MouseEventType::Exited)
    {
        dispatchExitEvent(event);
        return;
    }

    if (event.type == MouseEventType::Down)
    {
        dispatchMouseDown(target, event);
        return;
    }

    if (target != nullptr)
        target->handleMouseEvent(createLocalEvent(event, target, event.type));
}

void View::dispatchKeyEvent(const KeyEvent& event)
{
    // The single public entry for keyboard input (the native layer and the
    // debug server's input tools both call this), routing to the protected
    // handlers. Mirrors dispatchMouseEvent so no caller reaches keyDown/keyUp
    // directly.
    if (event.type == KeyEventType::Up)
        keyUp(event);
    else
        keyDown(event);
}

void View::handleMouseEvent(const MouseEvent& event)
{
    switch (event.type)
    {
        case MouseEventType::Down:
            if (properties.grabsFocusOnMouseDown)
                focus();
            mouseDown(event);
            break;
        case MouseEventType::Up:
            mouseUp(event);
            break;
        case MouseEventType::Dragged:
            mouseDragged(event);
            break;
        case MouseEventType::Moved:
            mouseMoved(event);
            break;
        case MouseEventType::Entered:
            mouseEntered(event);
            break;
        case MouseEventType::Exited:
            mouseExited(event);
            break;
        case MouseEventType::Wheel:
            mouseWheel(event);
            break;
    }
}

bool View::isHovering() const
{
    return getLocalBounds().contains(getMousePosition());
}

void View::addLayer(Layer& layer)
{
    if (layers.contains(&layer))
        return;

    layers.add(&layer);
    layer.attachTo(*this);
}

void View::removeLayer(Layer& layer)
{
    if (layers.removeAllMatches(&layer) > 0)
        layer.detachFromView();
}
} // namespace eacp::Graphics
