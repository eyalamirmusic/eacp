#pragma once

#include <eacp/Core/Utils/Common.h>
#include <eacp/Core/Utils/Containers.h>

#include <string>

#include "../Graphics/GraphicsContext.h"
#include "../Layers/Layer.h"
#include "../Graphics/Keyboard.h"

namespace eacp::Graphics
{

enum class MouseEventType
{
    Down,
    Up,
    Dragged,
    Moved,
    Entered,
    Exited,
    Wheel
};

enum class MouseButton
{
    Left = 0,
    Right = 1,
    Middle = 2,
    Other = 3
};

struct MouseEvent
{
    Point pos;
    Point downPos;
    Point delta;
    MouseEventType type = MouseEventType::Down;
    MouseButton button = MouseButton::Left;
    ModifierKeys modifiers;
    int clickCount = 1;
    float pressure = 1.0f;
    double timestamp = 0.0;
};

struct ViewProperties
{
    bool handlesMouseEvents = false;
    bool grabsFocusOnMouseDown = false;

    // When set, the view is dragged within its parent by the mouse — moved
    // by the pointer's motion straight from the dispatched events, so a real
    // mouse and an agent's synthetic input drag it through the identical
    // path. See View::setDraggable.
    bool draggable = false;
};

class View
{
    using ChildViews = std::initializer_list<std::reference_wrapper<View>>;

public:
    View();
    virtual ~View();

    void repaint();

    // Group opacity for the whole view subtree (chrome, child views and any
    // native GPU/web content), composited over whatever sits behind it. Sibling
    // of Layer::setOpacity, but for an entire View rather than a single layer.
    void setOpacity(float opacity);

    void* getHandle();

    virtual void paint(Context&) {};
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

    View& setHandlesMouseEvents(bool value = true);
    View& setGrabsFocusOnMouseDown(bool value = true);

    // Makes the view draggable within its parent: while held, it follows the
    // pointer, driven purely by the dispatched mouse events (no OS-cursor
    // side channel), so a human and an agent move it the same way. Implies
    // setHandlesMouseEvents(true). The view's own mouse handlers still fire,
    // so it can react to being dragged.
    View& setDraggable(bool value = true);

    Point getMousePosition() const;

    // A stable, app-assigned id — the native analogue of the WebView
    // @id / data-testid handle. Empty by default; apps tag the views they
    // want addressable from outside (tests, the debug server's list_views /
    // click_view tools). An id is only a locator: the tools resolve it to a
    // window point and drive through dispatchMouseEvent, so it never routes
    // around the hit-testing / handlesMouseEvents gate.
    View& setId(std::string newId);
    const std::string& getId() const { return viewId; }

    // Depth-first search of this view's subtree (self included) for the first
    // view with this id, or null. Ids aren't enforced unique — tree order wins.
    View* findChildById(const std::string& target);

    // This view's bounds in window (root) coordinates: getBounds() walked up
    // the parent chain. Turns a located view into a click point.
    Rect getWindowBounds() const;

    virtual View* hitTest(const Point& point);

    // The single public entry points for input — the exact ones the OS
    // native layer calls. Everything (real events, agent injection, tests)
    // goes through these, so input always passes the same hit-testing /
    // handlesMouseEvents gate. The per-view handlers below are protected so
    // nothing can route around that gate (which would let automation drive
    // a view a human can't).
    void dispatchMouseEvent(const MouseEvent& event);
    void dispatchKeyEvent(const KeyEvent& event);

    bool isHovering() const;

    void focus();
    bool hasFocus() const;

    const Vector<View*>& getSubviews() const { return subviews; }
    const Vector<Layer*>& getLayers() const { return layers; }
    View* getParent() const { return parent; }

    void* getNativeLayer();

protected:
    // Per-view input handlers — override to react. Deliberately NOT public:
    // input must enter through dispatchMouseEvent / dispatchKeyEvent (the
    // same entry the OS uses) so it always passes the view's hit-testing and
    // handlesMouseEvents gate. Making these public again would let an agent
    // or test drive a view a real mouse/keyboard can't reach — the exact
    // bug this guards against.
    virtual void mouseDown(const MouseEvent&) {}
    virtual void mouseUp(const MouseEvent&) {}
    virtual void mouseDragged(const MouseEvent&) {}
    virtual void mouseMoved(const MouseEvent&) {}
    virtual void mouseEntered(const MouseEvent&) {}
    virtual void mouseExited(const MouseEvent&) {}

    // Scroll wheel. event.delta carries the wheel movement (y vertical,
    // x horizontal) in WHEEL_DELTA units.
    virtual void mouseWheel(const MouseEvent&) {}
    virtual void keyDown(const KeyEvent&) {}
    virtual void keyUp(const KeyEvent&) {}

private:
    void handleMouseEvent(const MouseEvent& event);
    Point convertPointToDescendant(const Point& point, View* descendant) const;
    MouseEvent
        createLocalEvent(const MouseEvent& event, View* target, MouseEventType type);

    void forwardDragOrUpToCapturedTarget(const MouseEvent& event);
    void updateHoverTracking(View* target, const MouseEvent& event);
    void dispatchHoverEvent(View* target, const MouseEvent& event);
    void dispatchExitEvent(const MouseEvent& event);
    void dispatchMouseDown(View* target, const MouseEvent& event);

    void viewAdded(View& view);
    void viewRemoved(View& view);

    std::string viewId;
    Vector<View*> subviews;
    Vector<Layer*> layers;
    View* parent = nullptr;
    View* hoveredView = nullptr;
    View* mouseDownTarget = nullptr;

    // Pointer position (this view's coordinate space) at the last drag step,
    // for moving a draggable captured target by the pointer's motion.
    Point lastDragPos;

    // Latest dispatched pointer position (root view's coordinate space).
    // getMousePosition() reads this rather than the OS cursor, so it reflects
    // the same event stream real and synthetic input both flow through.
    Point lastPointerPos;

    ViewProperties properties;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics
