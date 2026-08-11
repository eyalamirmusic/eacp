#pragma once

#include "../Graphics/Graphics.h"
#include "KeyEvent.h"
#include "MouseEvent.h"

#include <functional>

namespace eacp::UI
{
class ComponentHost;
class DragAndDropTarget;
class DragAndDropContainer;

// A lightweight (non-native) UI element; a whole tree lives inside one
// ComponentHost. Bounds are logical points relative to the parent, and paint()
// is handed a Graphics already translated and clipped to getLocalBounds().
class Component
{
    using Children = std::initializer_list<std::reference_wrapper<Component>>;

public:
    Component();
    virtual ~Component();

    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    void setBounds(const Rect& newBounds);

    // Bounds as fractions of the parent's local bounds. Does nothing while
    // there is no parent.
    void setPos(const Rect& ratio);

    const Rect& getBounds() const { return bounds; }

    Rect getLocalBounds() const { return {0.f, 0.f, bounds.w, bounds.h}; }

    float getWidth() const { return bounds.w; }
    float getHeight() const { return bounds.h; }

    // Children are not owned: each must outlive its parent.
    void addAndMakeVisible(Component& child);
    void addChildren(Children childrenToAdd);

    // Adds without showing.
    void addChildComponent(Component& child);

    void removeChildComponent(Component& child);

    const Vector<Component*>& getChildren() const { return children; }
    Component* getParentComponent() const { return parent; }

    void setVisible(bool shouldBeVisible);
    bool isVisible() const { return visible; }

    // Z-order among siblings; last painted is on top.
    void toFront();
    void toBack();

    // Whether this component is a mouse target at all. Off by default.
    void setInterceptsMouseClicks(bool shouldIntercept);
    bool getInterceptsMouseClicks() const { return interceptsMouseClicks; }

    void setMouseCursor(eacp::Graphics::MouseCursor cursorToUse);
    eacp::Graphics::MouseCursor getMouseCursor() const { return cursor; }

    // The only thing that makes paint() run again: otherwise the recorded
    // DrawList is replayed. A move needs none (recordings are in local space), a
    // resize does - setBounds calls it. Called from paint(), marks the next frame.
    void repaint();

    // True from repaint() until the frame that answers it, and before the first
    // paint.
    bool needsRepaint() const { return selfDirty; }

    virtual void paint(Graphics&) {}

    // Drawn after the children, in this component's space.
    virtual void paintOverChildren(Graphics&) {}

    virtual void resized() {}

    // Rectangular by default.
    virtual bool hitTest(Point localPoint) const;

    virtual void mouseEnter(const MouseEvent&) {}
    virtual void mouseExit(const MouseEvent&) {}
    virtual void mouseDown(const MouseEvent&) {}
    virtual void mouseDrag(const MouseEvent&) {}
    virtual void mouseUp(const MouseEvent&) {}
    virtual void mouseMove(const MouseEvent&) {}

    // Return true to consume; unconsumed events carry on up the tree.
    virtual bool mouseWheelMove(const MouseEvent&) { return false; }

    // Off by default.
    void setWantsKeyboardFocus(bool shouldWantFocus);
    bool getWantsKeyboardFocus() const { return wantsKeyboardFocus; }

    // Also asks the native view for the keyboard. Does nothing on a component
    // that does not want focus.
    void grabKeyboardFocus();

    // Leaves the host with no focus, which sends keys to the root.
    void giveAwayKeyboardFocus();

    bool hasKeyboardFocus() const;

    virtual void focusGained() {}
    virtual void focusLost() {}

    // Return true to consume; unconsumed keys carry on up the parent chain, so
    // consuming a key an editor ignores kills Tab and window shortcuts.
    virtual bool keyDown(const KeyEvent&) { return false; }
    virtual bool keyUp(const KeyEvent&) { return false; }

    // The deepest visible, intercepting component under `localPoint`, or null.
    Component* getComponentAt(Point localPoint);

    Point localPointToRoot(Point localPoint) const;
    Point rootPointToLocal(Point rootPoint) const;

    // Null while this subtree is not in a host, the normal state during
    // construction.
    ComponentHost* getHost() const { return findHost(); }

    // Width of `text` in points, outside paint(). Zero while there is no host.
    float measureText(std::string_view text, const Font& font) const;

    // The host's font, zero-initialized while there is no host.
    Font getHostFont() const;

    bool isMouseOver() const { return mouseOver; }

    // In the order children were added, wrapping at the end of the tree. Null
    // when nothing in the tree wants the keyboard.
    Component* nextComponentWantingFocus(bool forwards = true);

    // Including this one.
    int countComponentsInTree() const;

    // Registered by PathShape's constructor; the host rasterizes the dirty ones
    // before the frame opens its render pass. Not owned.
    const Vector<PathShape*>& getPathShapes() const { return pathShapes; }

    // Registered by a DragAndDropTarget member's constructor; not owned, and
    // must outlive this component. Null when nothing can be dropped here.
    DragAndDropTarget* getDropTarget() const { return dropTarget; }

    // The nearest ancestor running drags, including this one, or null.
    DragAndDropContainer* findDragContainer() const;

    // Rendered into their own textures before the frame's pass opens, in
    // registration order - so a layer holding another must be constructed after
    // it. Not owned.
    const Vector<Layer*>& getLayers() const { return layers; }

private:
    friend class ComponentHost;
    friend class PathShape;
    friend class Layer;
    friend class DragAndDropTarget;
    friend class DragAndDropContainer;

    void setDropTarget(DragAndDropTarget* target) { dropTarget = target; }
    void setDragContainer(DragAndDropContainer* container)
    {
        dragContainer = container;
    }

    void addPathShape(PathShape& shape);
    void removePathShape(PathShape& shape);

    void addLayer(Layer& layer);
    void removeLayer(Layer& layer);

    ComponentHost* findHost() const;

    // Asks the host for a frame without marking any recording stale - for a
    // move, a reorder or a visibility change.
    void invalidateHost();

    // Stops at the first ancestor already marked; everything above it is too.
    void markAncestorsDirty();

    bool needsRecording() const { return selfDirty || descendantDirty; }

    Rect bounds;

    // What paint() and paintOverChildren produced, in this component's own
    // points. Kept until repaint() says they are stale.
    DrawList paintList;
    DrawList overList;

    // Invariant: a component with either bit set has descendantDirty set on
    // every one of its ancestors. repaint() maintains it going in, the recording
    // walk coming out - which is why that walk recomputes descendantDirty.
    bool selfDirty = true;
    bool descendantDirty = true;

    Vector<Component*> children;
    Vector<PathShape*> pathShapes;
    Vector<Layer*> layers;
    Component* parent = nullptr;

    DragAndDropTarget* dropTarget = nullptr;
    DragAndDropContainer* dragContainer = nullptr;

    // Set on a root only, by ComponentHost::setRootComponent; everything else
    // walks up to find it.
    ComponentHost* host = nullptr;

    bool visible = true;
    bool interceptsMouseClicks = false;
    bool wantsKeyboardFocus = false;
    bool mouseOver = false;

    eacp::Graphics::MouseCursor cursor = eacp::Graphics::MouseCursor::Default;
};
} // namespace eacp::UI
