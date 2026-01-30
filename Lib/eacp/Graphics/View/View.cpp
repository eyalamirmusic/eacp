#include "View.h"
#include <eacp/Core/Utils/Vectors.h>
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
    if (Vectors::contains(subviews, &view))
        return;

    view.removeFromParent();
    view.parent = this;
    subviews.push_back(&view);

    viewAdded(view);
}

void View::removeSubview(View& view)
{
    if (Vectors::eraseMatch(subviews, &view))
        viewRemoved(view);
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
        offset = point + offset;
        current = current->parent;
    }

    return point - offset;
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

void View::dispatchMouseEvent(const MouseEvent& event)
{
    auto* target = hitTest(event.pos);

    if (event.type == MouseEventType::Moved || event.type == MouseEventType::Entered)
    {
        if (target != hoveredView)
        {
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

        if (target != nullptr && event.type == MouseEventType::Moved)
        {
            target->handleMouseEvent(
                createLocalEvent(event, target, MouseEventType::Moved));
        }
    }
    else if (event.type == MouseEventType::Exited)
    {
        if (hoveredView != nullptr)
        {
            hoveredView->handleMouseEvent(
                createLocalEvent(event, hoveredView, MouseEventType::Exited));
            hoveredView = nullptr;
        }
    }
    else if (target != nullptr)
    {
        target->handleMouseEvent(createLocalEvent(event, target, event.type));
    }
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
    }
}

bool View::isHovering() const
{
    return getLocalBounds().contains(getMousePosition());
}



void View::addLayer(Layer& layer)
{
    if (Vectors::contains(layers, &layer))
        return;

    layers.push_back(&layer);
    layer.attachTo(*this);
}

void View::removeLayer(Layer& layer)
{
    if (Vectors::eraseMatch(layers, &layer))
        layer.detachFromView();
}
} // namespace eacp::Graphics
