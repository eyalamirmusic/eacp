#pragma once

#include "../Graphics/Graphics.h"
#include "KeyEvent.h"
#include "MouseEvent.h"

namespace eacp::UI
{
class ComponentHost;

// A lightweight UI element: bounds, children, paint, and mouse events.
//
// The deliberate difference from eacp::Graphics::View is that this is *not* a
// native object. A View is an NSView on macOS and a DirectComposition visual on
// Windows -- the right weight for a window's content, a web view or a GPU
// surface, and the wrong weight for a slider. A real interface has hundreds to
// thousands of elements, and one native view apiece would pay AppKit for
// layout, tracking and hit testing on every one, and would defeat the batching
// that makes drawing them cheap in the first place.
//
// So a whole tree of these lives inside a single ComponentHost, which is one
// GPUView, and is drawn in one pass. That is the same shape JUCE uses -- one
// peer per window, lightweight components inside it -- and it is what lets a
// component cost an allocation rather than a window-server object.
//
// Coordinates are logical points, and a component's bounds are relative to its
// parent. paint() is handed a Graphics already translated to this component's
// origin and clipped to its bounds, so a paint() body only ever thinks in
// getLocalBounds().
class Component
{
public:
    Component();
    virtual ~Component();

    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    void setBounds(const Rect& newBounds);
    const Rect& getBounds() const { return bounds; }

    // The bounds with the origin moved to zero -- what to lay children out
    // against, and what paint() draws in.
    Rect getLocalBounds() const { return {0.f, 0.f, bounds.w, bounds.h}; }

    float getWidth() const { return bounds.w; }
    float getHeight() const { return bounds.h; }

    // Adds `child` on top of the existing children and shows it. The child is
    // not owned: it has to outlive this component, which is what holding
    // children as members gets you for free.
    void addAndMakeVisible(Component& child);

    // Adds without showing, for a child whose visibility the caller drives.
    void addChildComponent(Component& child);

    void removeChildComponent(Component& child);

    const Vector<Component*>& getChildren() const { return children; }
    Component* getParentComponent() const { return parent; }

    void setVisible(bool shouldBeVisible);
    bool isVisible() const { return visible; }

    // Z-order among siblings. Last painted is on top, and hit testing walks the
    // same order reversed, so the two can never disagree.
    void toFront();
    void toBack();

    // Whether this component is a mouse target at all. Off by default: a panel
    // that only holds children should not swallow clicks meant for them, and
    // making that the default means a decorative component is inert without
    // anyone remembering to say so.
    void setInterceptsMouseClicks(bool shouldIntercept);
    bool getInterceptsMouseClicks() const { return interceptsMouseClicks; }

    void setMouseCursor(eacp::Graphics::MouseCursor cursorToUse);
    eacp::Graphics::MouseCursor getMouseCursor() const { return cursor; }

    // Marks the tree dirty. There is no partial-repaint bookkeeping: the host
    // redraws the whole tree, because with the draws batched that costs less
    // than tracking which rectangles changed. Nothing is drawn at all while
    // nothing is dirty, which is the part that matters for battery.
    void repaint();

    virtual void paint(Graphics&) {}

    // Drawn after the children, in this component's space -- a focus ring, a
    // drag overlay, anything that has to sit above a child.
    virtual void paintOverChildren(Graphics&) {}

    virtual void resized() {}

    // Whether `localPoint` counts as inside. Rectangular by default; override
    // for a round knob, or for a component with a transparent margin that
    // should let clicks through to what is behind it.
    virtual bool hitTest(Point localPoint) const;

    virtual void mouseEnter(const MouseEvent&) {}
    virtual void mouseExit(const MouseEvent&) {}
    virtual void mouseDown(const MouseEvent&) {}
    virtual void mouseDrag(const MouseEvent&) {}
    virtual void mouseUp(const MouseEvent&) {}
    virtual void mouseMove(const MouseEvent&) {}

    // Return true to consume the wheel event. Unconsumed, it carries on up the
    // tree, which is what a list needs: the pointer is over a row, and the row
    // does not scroll but the list holding it does. Returning a verdict rather
    // than forwarding by hand means a component that ignores the wheel needs no
    // code at all to let its parent have it.
    virtual bool mouseWheelMove(const MouseEvent&) { return false; }

    // Whether this component can hold keyboard focus. Off by default, the same
    // way mouse interception is and for the same reason: a panel that holds an
    // editor should not be able to take the keyboard away from it by accident.
    void setWantsKeyboardFocus(bool shouldWantFocus);
    bool getWantsKeyboardFocus() const { return wantsKeyboardFocus; }

    // Makes this the host's focused component, and asks the native view for the
    // keyboard while it is at it -- a component tree only sees a key event if the
    // one view it lives in is the window's first responder.
    void grabKeyboardFocus();

    // Gives it up, leaving the host with none. A tree with nothing focused sends
    // its keys to the root, which is what makes a shortcut work before anything
    // has been clicked.
    void giveAwayKeyboardFocus();

    bool hasKeyboardFocus() const;

    virtual void focusGained() {}
    virtual void focusLost() {}

    // Return true to consume. Unconsumed, a key carries on up the parent chain
    // exactly as the wheel does -- so a shortcut on a root works while an editor
    // deep inside it holds focus, and a component that ignores the keyboard
    // needs no code to pass one on.
    //
    // The reply matters more here than it looks: an editor consuming everything
    // it types must *not* consume the keys it ignores, or the tab that should
    // move focus and the shortcut that should reach the window die in it.
    virtual bool keyDown(const KeyEvent&) { return false; }
    virtual bool keyUp(const KeyEvent&) { return false; }

    // The deepest visible, intercepting component under `localPoint`, or null.
    // Front-to-back, so the topmost sibling wins.
    Component* getComponentAt(Point localPoint);

    Point localPointToRoot(Point localPoint) const;
    Point rootPointToLocal(Point rootPoint) const;

    bool isMouseOver() const { return mouseOver; }

    // The next component after this one that would take focus, in the order
    // children were added, wrapping at the end of the tree. What Tab moves to,
    // and null when nothing in the tree wants the keyboard at all.
    Component* nextComponentWantingFocus(bool forwards = true);

    // Every component in this subtree, including this one. The demo reports it
    // next to the draw count, since the claim being tested is that the second
    // does not grow with the first.
    int countComponentsInTree() const;

    // The vector shapes this component draws. A PathShape adds itself here in
    // its constructor, so the host can find every one in the tree and rasterize
    // the dirty ones before the frame opens its render pass - which is the only
    // point in a frame where a compute pass can run. See PathShape.
    const Vector<PathShape*>& getPathShapes() const { return pathShapes; }

    // The layers this component composites, found by the host the same way and
    // for the same reason: a layer's content is rendered into a texture of its
    // own before the frame's pass opens, a pass not being able to begin inside
    // another one. See Layer, and note its ordering rule -- these are rendered
    // in the order they registered, so a layer holding another has to be
    // constructed after it.
    const Vector<Layer*>& getLayers() const { return layers; }

private:
    friend class ComponentHost;
    friend class PathShape;
    friend class Layer;

    void addPathShape(PathShape& shape);
    void removePathShape(PathShape& shape);

    void addLayer(Layer& layer);
    void removeLayer(Layer& layer);

    ComponentHost* findHost() const;

    Rect bounds;
    Vector<Component*> children;
    Vector<PathShape*> pathShapes;
    Vector<Layer*> layers;
    Component* parent = nullptr;

    // Set on a root only, by ComponentHost::setRootComponent. Everything else
    // walks up to find it, so a subtree moved between hosts needs no fixing up.
    ComponentHost* host = nullptr;

    bool visible = true;
    bool interceptsMouseClicks = false;
    bool wantsKeyboardFocus = false;
    bool mouseOver = false;

    eacp::Graphics::MouseCursor cursor = eacp::Graphics::MouseCursor::Default;
};
} // namespace eacp::UI
