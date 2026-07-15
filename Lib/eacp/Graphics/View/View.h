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

    // Renders this view and its child views into an off-screen image, stacked
    // front-to-back the way the compositor draws them on screen: paint() chrome,
    // attached shape/text layers, GPU (Metal) content, and descendant views.
    // Embedded web content is async and NOT captured here (it comes out blank) --
    // use renderToImageAsync for that. scale is the pixels-per-point factor (2 on
    // a Retina display); pass 0 to use the view's current backing scale. Returns
    // an invalid Image for a non-positive size.
    Image renderToImage(float scale = 0.0f);

    // Like renderToImage, but also folds in embedded WebView content, which the
    // web runtime only yields asynchronously. Resolves on the main thread once
    // every descendant WebView has been snapshotted. Prefer the synchronous
    // renderToImage when the subtree has no web content.
    Threads::Async<Image> renderToImageAsync(float scale = 0.0f);

    // Group opacity for the whole view subtree (chrome, child views and any
    // native GPU/web content), composited over whatever sits behind it. Sibling
    // of Layer::setOpacity, but for an entire View rather than a single layer.
    void setOpacity(float opacity);
    float getOpacity() const { return opacity; }

    void* getHandle();

    virtual void paint(Context&) {};

    // Native, non-paint content this view renders itself (a GPUView's Metal
    // layer), returned as a straight-alpha Image sized to the view's bounds at
    // `scale` pixels per point. The snapshot compositor draws the result over
    // paint()/layers, since renderInContext: cannot reach such content. Default
    // is none (invalid Image). WebView content is async and handled separately.
    virtual Image renderNativeContent(float scale);

    // Zero-copy variant of renderNativeContent for video capture: renders this
    // view's native GPU content straight into a platform frame target instead of
    // reading it back to an Image. On Apple `nativeTarget` is a CVPixelBufferRef
    // whose IOSurface the view renders into; the pixels never leave the GPU.
    // Returns false when the view has no such content (the default) -- GPUView
    // overrides it. `scale` matches renderToImage's.
    virtual bool renderNativeContentToTarget(void* nativeTarget, float scale);

    // Async native content (a WebView's page), which the web runtime only yields
    // via a callback. hasAsyncContent() reports whether this view has any;
    // captureAsyncContent() delivers it as a straight-alpha Image sized to the
    // view's bounds at `scale`, invoking done on the main thread (with an invalid
    // Image on failure). renderToImageAsync folds the result into the snapshot.
    virtual bool hasAsyncContent() const { return false; }
    virtual void captureAsyncContent(float scale, std::function<void(Image)> done);

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
    float opacity = 1.0f;
    View* parent = nullptr;
    View* hoveredView = nullptr;
    View* mouseDownTarget = nullptr;

    ViewProperties properties;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics
