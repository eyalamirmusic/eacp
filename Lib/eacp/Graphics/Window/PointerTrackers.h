#pragma once

#include "../View/View.h"

// What a pointer's events mean when the system that carried them says only
// where and which button: the click count and the slop it allows, the scroll a
// frame adds up to and the cursor shape. Protocol-free, so the Linux seats and
// the web's DOM events feed the same ones.

namespace eacp::Graphics
{
// What no window system reports: the click count and the slop it allows, the
// button held down and where it went down.
class PointerTracker
{
public:
    constexpr Point getPosition() const { return position; }
    constexpr void setPosition(Point newPosition) { position = newPosition; }

    // The click count this press earns.
    int pressed(MouseButton button, uint32_t timeMilliseconds);
    constexpr void released() { buttonHeld = false; }

    constexpr bool isButtonHeld() const { return buttonHeld; }
    constexpr MouseButton getHeldButton() const { return heldButton; }
    constexpr Point getDownPosition() const { return downPosition; }
    constexpr int getClickCount() const { return clickCount; }

private:
    Point position;
    Point downPosition;

    bool buttonHeld = false;
    MouseButton heldButton = MouseButton::Left;

    int clickCount = 0;
    uint32_t lastClickTime = 0;
    MouseButton lastClickButton = MouseButton::Left;
    Point lastClickPosition;
};

// One frame's worth of scroll, however the server spelled it out.
class WheelTracker
{
public:
    // Both in MouseEvent::delta's orientation, not the server's.
    void addDelta(Point delta);
    void addNotches(Point notches);

    void setSource(bool isPrecise, bool isGesture);
    void setStopped();

    constexpr bool isPending() const { return pending; }
    constexpr bool isPrecise() const { return precise; }
    constexpr bool isGesture() const { return gesture; }
    constexpr bool hasStopped() const { return stopped; }

    // Lines for a notched wheel, points for a trackpad.
    Point getDelta() const;

    // Keeps the source, which is a property of the device and not of a frame.
    void endFrame();

private:
    Point delta;
    Point notches;
    bool hasNotches = false;
    bool pending = false;
    bool precise = false;
    bool gesture = false;
    bool stopped = false;
};

// The shape under the pointer, and whether a mouse lock has hidden it. Both
// setters are true when the backend must apply the cursor again.
class CursorTracker
{
public:
    bool setShape(MouseCursor::Shape newShape);
    bool setHidden(bool shouldHide);

    constexpr MouseCursor::Shape getShape() const { return shape; }
    constexpr bool isHidden() const { return hidden; }

private:
    MouseCursor::Shape shape = MouseCursor::Default;
    bool hidden = false;
};
} // namespace eacp::Graphics
