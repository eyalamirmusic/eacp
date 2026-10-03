#include "View.h"
#include "../Image/Image.h"
#include "../Window/Window.h"
#include <algorithm>
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

View& View::setHandlesTouchEvents(bool value)
{
    properties.handlesTouchEvents = value;
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

Window* View::getWindow() const
{
    // Only the view a window adopted carries the pointer, so a subview walks up
    // to find it. Reading it off the root rather than caching it per view is
    // what makes a subtree moved from one window to another need no bookkeeping:
    // the answer follows the parent chain it already moved along.
    const View* root = this;

    while (root->parent != nullptr)
        root = root->parent;

    return root->ownerWindow;
}

void View::resized() {}

void View::notifyVisibilityChanged(bool effectivelyVisible)
{
    visibilityChanged(effectivelyVisible);

    for (auto* child: subviews)
        if (child->visible)
            child->notifyVisibilityChanged(effectivelyVisible);
}

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

        forgetTouchesIn(view);
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

void View::dispatchKeyEvent(const KeyEvent& event)
{
    if (auto* window = getWindow())
        window->events.input.keyEvent(event);

    if (event.type == KeyEventType::Down)
        keyDown(event);
    else
        keyUp(event);
}

void View::dispatchMouseEvent(const MouseEvent& event)
{
    if (ownerWindow != nullptr)
        ownerWindow->events.input.mouseEvent(event);

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

View* View::touchTarget(const Point& point)
{
    if (!getLocalBounds().contains(point))
        return nullptr;

    for (auto child: std::ranges::reverse_view(subviews))
    {
        auto childBounds = child->getBounds();
        auto childPoint = Point {point.x - childBounds.x, point.y - childBounds.y};

        if (auto* hit = child->touchTarget(childPoint))
            return hit;
    }

    if (properties.handlesTouchEvents || properties.handlesMouseEvents)
        return this;

    return nullptr;
}

void View::dispatchTouchEvent(const TouchEvent& event)
{
    if (event.phase == TouchPhase::Began)
    {
        beginTouch(event);
        return;
    }

    auto* touch =
        touches.findIf([&](const ActiveTouch& t) { return t.id == event.id; });

    if (touch == nullptr)
        return;

    auto withDown = event;
    withDown.downPos = touch->downPos;

    if (touch->view != nullptr)
        sendTouch(*touch->view, withDown);
    else
        sendTouchAsMouse(withDown);

    if (event.phase == TouchPhase::Ended || event.phase == TouchPhase::Cancelled)
        touches.removeIndexesMatching([&](const ActiveTouch& t)
                                      { return t.id == event.id; });
}

void View::beginTouch(const TouchEvent& event)
{
    touches.removeIndexesMatching([&](const ActiveTouch& t)
                                  { return t.id == event.id; });

    auto withDown = event;
    withDown.downPos = event.pos;

    auto* target = touchTarget(event.pos);

    if (target != nullptr && target->properties.handlesTouchEvents)
    {
        touches.add({event.id, target, event.pos});
        sendTouch(*target, withDown);
        return;
    }

    auto mouseIsTaken =
        touches.findIf([](const ActiveTouch& t) { return t.view == nullptr; })
        != nullptr;

    if (mouseIsTaken)
        return;

    touches.add({event.id, nullptr, event.pos});
    sendTouchAsMouse(withDown);
}

void View::sendTouch(View& target, const TouchEvent& event)
{
    auto local = event;
    local.pos = convertPointToDescendant(event.pos, &target);
    local.downPos = convertPointToDescendant(event.downPos, &target);

    switch (event.phase)
    {
        case TouchPhase::Began:
            target.touchBegan(local);
            break;
        case TouchPhase::Moved:
            target.touchMoved(local);
            break;
        case TouchPhase::Ended:
        case TouchPhase::Cancelled:
            target.touchEnded(local);
            break;
    }
}

void View::sendTouchAsMouse(const TouchEvent& event)
{
    auto mouse = MouseEvent {};
    mouse.pos = event.pos;
    mouse.downPos = event.downPos;
    mouse.button = MouseButton::Left;
    mouse.timestamp = event.timestamp;

    switch (event.phase)
    {
        case TouchPhase::Began:
            mouse.type = MouseEventType::Down;
            break;
        case TouchPhase::Moved:
            mouse.type = MouseEventType::Dragged;
            break;
        case TouchPhase::Ended:
        case TouchPhase::Cancelled:
            mouse.type = MouseEventType::Up;
            break;
    }

    if (mouse.type != MouseEventType::Dragged)
    {
        mouse.pressure = event.pressure;
        mouse.clickCount = event.tapCount;
    }

    dispatchMouseEvent(mouse);
}

void View::forgetTouchesIn(View& removed)
{
    auto* root = this;

    while (root->parent != nullptr)
        root = root->parent;

    auto isInRemoved = [&removed](const View* view)
    {
        for (; view != nullptr; view = view->parent)
            if (view == &removed)
                return true;

        return false;
    };

    root->touches.removeIndexesMatching([&](const ActiveTouch& t)
                                        { return isInRemoved(t.view); });
}

Insets View::getSafeAreaInsets() const
{
    if (parent == nullptr)
        return safeAreaInsets;

    auto outer = parent->getSafeAreaInsets();
    auto bounds = getBounds();
    auto parentBounds = parent->getLocalBounds();

    return {.top = std::max(0.f, outer.top - bounds.y),
            .left = std::max(0.f, outer.left - bounds.x),
            .bottom =
                std::max(0.f, outer.bottom - (parentBounds.h - bounds.bottom())),
            .right = std::max(0.f, outer.right - (parentBounds.w - bounds.right()))};
}

void View::setSafeAreaInsets(const Insets& insets)
{
    if (insets.top == safeAreaInsets.top && insets.left == safeAreaInsets.left
        && insets.bottom == safeAreaInsets.bottom
        && insets.right == safeAreaInsets.right)
        return;

    safeAreaInsets = insets;
    notifySafeAreaInsetsChanged();
}

void View::notifySafeAreaInsetsChanged()
{
    safeAreaInsetsChanged();

    for (auto* child: subviews)
        child->notifySafeAreaInsetsChanged();
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
