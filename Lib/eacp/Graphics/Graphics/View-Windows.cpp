#include "View.h"
#include "ShapeLayer.h"
#include "TextLayer.h"
#include "NativeLayer-Windows.h"

#include <cassert>

#define NOMINMAX
#include <Windows.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

// Forward declarations
wuc::Compositor getWinRTCompositor();

struct View::Native
{
    Native(View* owner)
        : ownerView(owner)
    {
        // Create container visual for this view
        auto compositor = getWinRTCompositor();
        if (compositor)
        {
            visual = compositor.CreateContainerVisual();
        }
    }

    ~Native()
    {
        detachFromParent();
        visual = nullptr;
    }

    void attachToParent(wuc::ContainerVisual parentVisual)
    {
        if (parentVisual && visual)
        {
            parentVisual.Children().InsertAtTop(visual);
            parent = parentVisual;
        }
    }

    void detachFromParent()
    {
        if (parent && visual)
        {
            parent.Children().Remove(visual);
            parent = nullptr;
        }
    }

    void updateVisualPosition()
    {
        if (visual)
        {
            visual.Offset({bounds.x, bounds.y, 0.0f});
        }
    }

    wuc::ContainerVisual getVisual() { return visual; }

    View* ownerView;
    Rect bounds;
    bool hasFocus = false;
    bool isHovering = false;
    wuc::ContainerVisual visual {nullptr};
    wuc::ContainerVisual parent {nullptr};
};

View::View()
    : impl(this)
{
}

View::~View()
{
    // Remove from parent if attached
    if (parent)
    {
        parent->removeSubview(*this);
    }

    // Clear subviews
    for (auto* subview: subviews)
    {
        subview->parent = nullptr;
    }
    subviews.clear();
}

void View::repaint()
{
    // Find the root window and invalidate
    // On Windows, we need to find the HWND and invalidate it
    // For now, this is a no-op as we don't have direct HWND access from View
}

void* View::getHandle()
{
    // On Windows, return a pointer to the ContainerVisual
    // This allows Window to attach the content view's visual to the root visual
    static wuc::ContainerVisual visualCopy {nullptr};
    visualCopy = impl->getVisual();
    return &visualCopy;
}

void View::resized()
{
    // Default implementation does nothing
    // Subclasses override this
}

Rect View::getBounds() const
{
    return impl->bounds;
}

Rect View::getLocalBounds() const
{
    return Rect(0, 0, impl->bounds.w, impl->bounds.h);
}

Rect View::getRelativeBounds(const Rect& ratio) const
{
    return getLocalBounds().getRelative(ratio);
}

void View::setBounds(const Rect& bounds)
{
    impl->bounds = bounds;
    impl->updateVisualPosition();
    resized();

    // Update subview bounds if they were set relatively
    // Subviews maintain their own bounds
}

void View::setBoundsRelative(const Rect& ratio)
{
    if (parent)
    {
        setBounds(parent->getLocalBounds().getRelative(ratio));
    }
}

void View::scaleToFit()
{
    // Scale all layers to fit local bounds
    for (auto* layer: layers)
    {
        layer->setBounds(getLocalBounds());
    }
}

void View::scaleToFit(ChildViews views)
{
    for (auto& viewRef: views)
    {
        viewRef.get().setBounds(getLocalBounds());
    }
}

void View::addChildren(ChildViews views)
{
    for (auto& viewRef: views)
    {
        addSubview(viewRef.get());
    }
}

void View::addSubview(View& view)
{
    view.parent = this;
    subviews.push_back(&view);

    // Attach child visual to this view's visual
    if (impl->visual && view.impl->visual)
    {
        view.impl->attachToParent(impl->getVisual());
        view.impl->updateVisualPosition();
    }
}

void View::removeSubview(View& view)
{
    auto it = std::find(subviews.begin(), subviews.end(), &view);
    if (it != subviews.end())
    {
        // Detach child visual from this view's visual
        view.impl->detachFromParent();
        (*it)->parent = nullptr;
        subviews.erase(it);
    }
}

void View::removeFromParent()
{
    if (parent)
    {
        parent->removeSubview(*this);
    }
}

void View::addLayer(Layer& layer)
{
    layers.push_back(&layer);
    // Pass the view's ContainerVisual to the layer for attachment
    layer.attachToLayer(&impl->visual);
}

void View::removeLayer(Layer& layer)
{
    auto it = std::find(layers.begin(), layers.end(), &layer);
    if (it != layers.end())
    {
        (*it)->detachFromLayer();
        layers.erase(it);
    }
}

Point View::getMousePosition() const
{
    POINT pt;
    GetCursorPos(&pt);
    // TODO: Convert to view coordinates
    return Point(static_cast<float>(pt.x), static_cast<float>(pt.y));
}

View* View::hitTest(const Point& point)
{
    // Check if point is within bounds
    if (!getLocalBounds().contains(point))
    {
        return nullptr;
    }

    // Check subviews in reverse order (top to bottom)
    for (auto it = subviews.rbegin(); it != subviews.rend(); ++it)
    {
        View* subview = *it;
        // Convert point to subview coordinates
        Point localPoint(point.x - subview->getBounds().x,
                         point.y - subview->getBounds().y);
        View* hit = subview->hitTest(localPoint);
        if (hit)
        {
            return hit;
        }
    }

    // If this view handles mouse events, return this
    if (properties.handlesMouseEvents)
    {
        return this;
    }

    return nullptr;
}

void View::dispatchMouseEvent(const MouseEvent& event)
{
    View* target = hitTest(event.pos);

    if (event.type == MouseEventType::Moved)
    {
        // Handle enter/exit events
        if (target != hoveredView)
        {
            if (hoveredView)
            {
                MouseEvent exitEvent = event;
                exitEvent.type = MouseEventType::Exited;
                hoveredView->handleMouseEvent(exitEvent);
            }
            hoveredView = target;
            if (hoveredView)
            {
                MouseEvent enterEvent = event;
                enterEvent.type = MouseEventType::Entered;
                hoveredView->handleMouseEvent(enterEvent);
            }
        }
    }

    if (target)
    {
        MouseEvent localEvent = createLocalEvent(event, target, event.type);
        target->handleMouseEvent(localEvent);
    }
}

void View::handleMouseEvent(const MouseEvent& event)
{
    switch (event.type)
    {
        case MouseEventType::Down:
            if (properties.grabsFocusOnMouseDown)
            {
                focus();
            }
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
            impl->isHovering = true;
            mouseEntered(event);
            break;
        case MouseEventType::Exited:
            impl->isHovering = false;
            mouseExited(event);
            break;
    }
}

Point View::convertPointToDescendant(const Point& point, View* descendant)
{
    Point result = point;
    View* current = this;

    // Walk down the view hierarchy to find the path to descendant
    while (current != descendant && current != nullptr)
    {
        for (auto* subview: current->subviews)
        {
            // Check if descendant is in this subtree
            // For simplicity, just adjust by bounds
            if (subview == descendant)
            {
                result.x -= subview->getBounds().x;
                result.y -= subview->getBounds().y;
                return result;
            }
        }
        // If not found, search deeper (simplified)
        break;
    }

    return result;
}

MouseEvent View::createLocalEvent(const MouseEvent& event,
                                  View* target,
                                  MouseEventType type)
{
    MouseEvent localEvent = event;
    localEvent.type = type;

    // Convert position to target's local coordinates
    // Walk up from target to find offset
    Point offset(0, 0);
    View* current = target;
    while (current && current != this)
    {
        offset.x += current->getBounds().x;
        offset.y += current->getBounds().y;
        current = current->parent;
    }

    localEvent.pos.x = event.pos.x - offset.x;
    localEvent.pos.y = event.pos.y - offset.y;

    return localEvent;
}

bool View::isHovering() const
{
    return impl->isHovering;
}

void View::focus()
{
    impl->hasFocus = true;
}

bool View::hasFocus() const
{
    return impl->hasFocus;
}

} // namespace eacp::Graphics
