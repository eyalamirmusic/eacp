#pragma once

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

    // How far the pointer moved, in points — the movement the system would
    // have given the cursor, shaped by its acceleration curve. That curve
    // exists so a cursor can cross a screen and still land on a target, and it
    // is what a widget dragged by the pointer should follow: an infinite-drag
    // knob or scrubber moves with the hand's *pointer*, not its device.
    Point delta;

    // How far the device itself moved, with no acceleration curve applied.
    // Linear: the same physical movement always reports the same figure,
    // however fast it was made, which is what a camera needs — an FPS look, a
    // 3D orbit. Applying the pointer's curve to a camera makes an identical
    // flick of the hand turn different amounts depending on its speed, which
    // reads as the aim being unpredictable.
    //
    // In the device's own units (mouse counts), not points: it does not scale
    // with the display, so a sensitivity tuned against it stays put across
    // monitors. Falls back to `delta` where the platform cannot report it.
    Point rawDelta;

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

    Point getMousePosition() const;

    virtual View* hitTest(const Point& point);

    void dispatchMouseEvent(const MouseEvent& event);

    bool isHovering() const;

    void focus();
    bool hasFocus() const;

    // The native view that should receive keyboard focus when this view is a
    // window's content view and the window becomes key. Defaults to this
    // view's own backing view. WebView overrides it so the nested platform web
    // view is focused rather than the empty container that hosts it — without
    // that, the page never gets key focus and its inputs stay dead until
    // clicked directly. See Window's key-activation handling.
    virtual void* nativeFocusTarget();

    const Vector<View*>& getSubviews() const { return subviews; }
    const Vector<Layer*>& getLayers() const { return layers; }
    View* getParent() const { return parent; }

    void* getNativeLayer();

private:
    void handleMouseEvent(const MouseEvent& event);
    Point convertPointToDescendant(const Point& point, View* descendant);
    MouseEvent
        createLocalEvent(const MouseEvent& event, View* target, MouseEventType type);

    void forwardDragOrUpToCapturedTarget(const MouseEvent& event);
    void updateHoverTracking(View* target, const MouseEvent& event);
    void dispatchHoverEvent(View* target, const MouseEvent& event);
    void dispatchExitEvent(const MouseEvent& event);
    void dispatchMouseDown(View* target, const MouseEvent& event);

    void viewAdded(View& view);
    void viewRemoved(View& view);

    Vector<View*> subviews;
    Vector<Layer*> layers;
    View* parent = nullptr;
    View* hoveredView = nullptr;
    View* mouseDownTarget = nullptr;

    ViewProperties properties;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics
