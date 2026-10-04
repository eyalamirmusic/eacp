#pragma once

#include "../View/View.h"

namespace eacp::Graphics
{

// Sees every key and mouse event the platform hands a window, before its views
// do, and every change of the window's key focus. For input layers that sit
// beside the view tree rather than in it (GameInput), so they need no view's
// cooperation. Everything arrives on the main thread.
struct WindowInputListener
{
    virtual ~WindowInputListener() = default;

    virtual void windowKeyEvent(const KeyEvent&);
    virtual void windowMouseEvent(const MouseEvent&);
    virtual void windowActivationChanged(bool);
};

// The platform layer reports into this; listeners subscribe to it. A listener
// must remove itself before it is destroyed, and the window must outlive it.
class WindowInputTap
{
public:
    void addListener(WindowInputListener& listener);
    void removeListener(WindowInputListener& listener);

    void keyEvent(const KeyEvent& event) const;
    void mouseEvent(const MouseEvent& event) const;
    void activationChanged(bool isKey);

    // Whether the window last reported having key focus.
    constexpr bool isActive() const { return active; }

private:
    Vector<WindowInputListener*> listeners;
    bool active = false;
};

} // namespace eacp::Graphics
