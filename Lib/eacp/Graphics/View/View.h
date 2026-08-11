#pragma once

#include "../Graphics/GraphicsContext.h"
#include "../Layers/Layer.h"
#include "../Graphics/Keyboard.h"

#include <functional>

namespace eacp::Threads
{
template <typename T>
class Async;
}

namespace eacp::Graphics
{

class Image;
class Window;

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

enum class MouseCursor
{
    Default,
    IBeam,
    PointingHand,
    ResizeLeftRight,
    ResizeUpDown,
    Crosshair
};

// A trackpad runs Began -> Changed -> Ended while the fingers are down, then
// Momentum while the motion coasts; a notched wheel always reports None.
enum class ScrollPhase
{
    None,
    Began,
    Changed,
    Ended,
    Momentum,
    MomentumEnded
};

struct MouseEvent
{
    Point pos;
    Point downPos;

    // Pointer movement in points, with the system acceleration curve applied.
    Point delta;

    // Unaccelerated device movement in mouse counts, not points. Falls back to
    // `delta` where the platform cannot report it.
    Point rawDelta;

    MouseEventType type = MouseEventType::Down;
    MouseButton button = MouseButton::Left;
    ModifierKeys modifiers;
    int clickCount = 1;
    float pressure = 1.0f;
    double timestamp = 0.0;

    // Wheel events only: true when `delta` is in points (trackpad), false when
    // in lines (notched wheel). Positive y means the content moves down; the
    // user's natural-scroll preference is already applied, so do not invert it.
    bool preciseScrolling = false;

    // Wheel events only.
    ScrollPhase scrollPhase = ScrollPhase::None;
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

    // Off-screen snapshot of this view's subtree. `scale` is pixels per point
    // (0 = the view's current backing scale). Embedded WebView content comes
    // out blank here; use renderToImageAsync for that.
    Image renderToImage(float scale = 0.0f);

    // renderToImage plus embedded WebView content; resolves on the main thread.
    Threads::Async<Image> renderToImageAsync(float scale = 0.0f);

    // Group opacity for the whole subtree, including native GPU/web content.
    void setOpacity(float opacity);
    float getOpacity() const { return opacity; }

    void* getHandle();

    virtual void paint(Context&) {};

    // Native GPU content (a GPUView's Metal layer) as a straight-alpha Image
    // sized to the view's bounds at `scale` pixels per point; invalid when the
    // view has none.
    virtual Image renderNativeContent(float scale);

    // Zero-copy variant for video capture: `nativeTarget` is a CVPixelBufferRef
    // on Apple. False when the view has no native GPU content.
    virtual bool renderNativeContentToTarget(void* nativeTarget, float scale);

    // A WebView's page, which the web runtime only yields via a callback.
    // `done` runs on the main thread, with an invalid Image on failure.
    virtual bool hasAsyncContent() const { return false; }
    virtual void captureAsyncContent(float scale, std::function<void(Image)> done);

    virtual void mouseDown(const MouseEvent&) {}
    virtual void mouseUp(const MouseEvent&) {}
    virtual void mouseDragged(const MouseEvent&) {}
    virtual void mouseMoved(const MouseEvent&) {}
    virtual void mouseEntered(const MouseEvent&) {}
    virtual void mouseExited(const MouseEvent&) {}

    // event.delta carries the wheel movement in WHEEL_DELTA units.
    virtual void mouseWheel(const MouseEvent&) {}
    virtual void keyDown(const KeyEvent&) {}
    virtual void keyUp(const KeyEvent&) {}
    virtual void resized();

    // The display's pixels-per-point changed; rebuild anything sized in device
    // pixels rather than logical points.
    virtual void backingScaleChanged() {}

    // Only views backed by a native surface the OS places in screen coordinates
    // (a WebView) need these; composited views follow the window for free.
    virtual void hostWindowMoved() {}
    virtual void hostWindowVisibilityChanged(bool) {}

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

    // Settable at any time, including from inside mouseMoved; setting the same
    // shape twice is free.
    void setMouseCursor(MouseCursor cursor);
    MouseCursor getMouseCursor() const { return currentCursor; }

    virtual View* hitTest(const Point& point);

    void dispatchMouseEvent(const MouseEvent& event);

    bool isHovering() const;

    void focus();
    bool hasFocus() const;

    // Native view to focus when this view is a window's content view and the
    // window becomes key; defaults to this view's own backing view.
    virtual void* nativeFocusTarget();

    const Vector<View*>& getSubviews() const { return subviews; }
    const Vector<Layer*>& getLayers() const { return layers; }
    View* getParent() const { return parent; }

    // Walks up to the root. Null before Window::setContentView, inside an
    // EmbeddedView, or once the window has been destroyed.
    Window* getWindow() const;

    void* getNativeLayer();

private:
    // Only ever set on a root view, by Window::setContentView, and cleared when
    // that window is destroyed. See Window::ContentViewLink.
    friend class Window;
    Window* ownerWindow = nullptr;

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
    float opacity = 1.0f;
    View* parent = nullptr;
    View* hoveredView = nullptr;
    View* mouseDownTarget = nullptr;

    ViewProperties properties;

    MouseCursor currentCursor = MouseCursor::Default;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics
