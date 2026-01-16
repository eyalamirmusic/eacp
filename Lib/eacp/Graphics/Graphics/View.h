#pragma once

#include <eacp/Core/Utils/Common.h>
#include "GraphicsContext.h"
#include "Layer.h"

namespace eacp::Graphics
{

enum class MouseEventType
{
    Down,
    Up,
    Dragged,
    Moved,
    Entered,
    Exited
};

struct MouseEvent
{
    Point pos;
    MouseEventType type = MouseEventType::Down;
};

struct ViewProperties
{
    bool handlesMouseEvents = false;
};

class View
{
    using ChildViews = std::initializer_list<std::reference_wrapper<View>>;

public:
    View();
    virtual ~View();

    void repaint();
    void* getHandle();

    virtual void paint(Context&) {};
    virtual void mouseDown(const MouseEvent&) {}
    virtual void mouseUp(const MouseEvent&) {}
    virtual void mouseDragged(const MouseEvent&) {}
    virtual void mouseMoved(const MouseEvent&) {}
    virtual void mouseEntered(const MouseEvent&) {}
    virtual void mouseExited(const MouseEvent&) {}
    virtual void resized();

    Rect getBounds() const;
    Rect getLocalBounds() const;

    Rect getRelativeBounds(const Rect& ratio) const;

    void setBounds(const Rect& bounds);
    void setBoundsRelative(const Rect& ratio);

    void scaleToFit();
    void scaleToFit(ChildViews views);

    void addChildren(ChildViews views);
    void addSubview(View& view);
    void removeSubview(View& view);
    void removeFromParent();

    void addLayer(Layer& layer);
    void removeLayer(Layer& layer);

    ViewProperties& getProperties() { return properties; }

    Point getMousePosition() const;

    virtual View* hitTest(const Point& point);

    void dispatchMouseEvent(const MouseEvent& event);

    bool isHovering() const;

private:
    void handleMouseEvent(const MouseEvent& event);
    Point convertPointToDescendant(const Point& point, View* descendant);
    std::vector<View*> subviews;
    std::vector<Layer*> layers;
    View* parent = nullptr;
    View* hoveredView = nullptr; // Tracks which view mouse is currently over

    ViewProperties properties;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics