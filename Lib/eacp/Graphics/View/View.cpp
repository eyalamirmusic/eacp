#include "View.h"
#include "../Image/Image.h"
#include <ranges>

namespace eacp::Graphics
{
Image View::renderNativeContent(float)
{
    return {};
}

bool View::renderNativeContentToTarget(void*, float)
{
    return false;
}

void View::captureAsyncContent(float, std::function<void(Image)> done)
{
    done({});
}

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

void* View::nativeFocusTarget()
{
    return getHandle();
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
        clearMouseTargetsIn(view);
        viewRemoved(view);
    }
}

bool View::isInSubtreeOf(View* candidate, View& subtreeRoot)
{
    for (auto* current = candidate; current != nullptr; current = current->parent)
    {
        if (current == &subtreeRoot)
            return true;
    }

    return false;
}

// hoveredView/mouseDownTarget are recorded on whichever view the platform
// event arrived at -- usually the window root -- and can point anywhere below
// it. Clearing them on the whole ancestor chain keeps them from dangling when
// the removed subtree is destroyed.
void View::clearMouseTargetsIn(View& view)
{
    for (auto* ancestor = this; ancestor != nullptr; ancestor = ancestor->parent)
    {
        if (isInSubtreeOf(ancestor->hoveredView, view))
            ancestor->hoveredView = nullptr;

        if (isInSubtreeOf(ancestor->mouseDownTarget, view))
            ancestor->mouseDownTarget = nullptr;
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

Point View::convertPointToDescendant(const Point& point, View* descendant)
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
        target->handleMouseEvent(createLocalEvent(event, target, event.type));
}

void View::dispatchMouseEvent(const MouseEvent& event)
{
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
