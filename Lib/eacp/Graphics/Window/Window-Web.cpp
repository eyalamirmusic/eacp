#include "Window.h"

#include "LinuxWindowNative-Linux.h"
#include "LinuxWindowSystem-Linux.h"
#include "PointerTrackers.h"
#include "WebWindowSystem-Web.h"
#include "../Graphics/Keyboard-Web.h"
#include "../View/WebViewSurface-Web.h"

#include <eacp/Core/App/AppEnvironment.h>

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

// A window is a <canvas>: the page's own (Module.canvas) for the first one, a
// canvas of its own for every other, made, sized and shown by the window. The
// DOM half is Window-Web.js, which forwards the canvas's pointer, wheel, key
// and focus events to the eacp_web_window_* functions at the bottom; what
// they mean is decided here, through the Linux-neutral state machines. With
// no document - node - or a headless app environment, a window is the Linux
// headless native: laid out, never shown.

namespace eacp::Graphics
{
namespace
{
enum class WebPointerEvent
{
    Entered,
    Left,
    Moved,
    Pressed,
    Released
};

// What a deltaMode of 2 (a page per notch) counts as in lines.
constexpr float webLinesPerPage = 10.f;

class WebWindowNative;

// One pointer and one keyboard per page, as a Linux seat has.
struct WebSeat
{
    WebWindowNative* pointerWindow = nullptr;
    PointerTracker pointer;
    WheelTracker wheel;
    CursorTracker cursor;
};

WebSeat& webSeat()
{
    static auto seat = WebSeat {};
    return seat;
}

std::unordered_map<int, WebWindowNative*>& webWindows()
{
    static auto windows = std::unordered_map<int, WebWindowNative*> {};
    return windows;
}

int nextWebWindowId()
{
    static auto next = 0;
    return ++next;
}

std::string takeWebString(char* text)
{
    if (text == nullptr)
        return {};

    auto result = std::string(text);
    std::free(text);
    return result;
}

const char* webCursorName(MouseCursor::Shape shape)
{
    switch (shape)
    {
        case MouseCursor::IBeam:
            return "text";
        case MouseCursor::PointingHand:
            return "pointer";
        case MouseCursor::ResizeLeftRight:
            return "ew-resize";
        case MouseCursor::ResizeUpDown:
            return "ns-resize";
        case MouseCursor::Crosshair:
            return "crosshair";
        case MouseCursor::Default:
        default:
            return "default";
    }
}

MouseButton webButtonFromDom(int button)
{
    switch (button)
    {
        case 0:
            return MouseButton::Left;
        case 1:
            return MouseButton::Middle;
        case 2:
            return MouseButton::Right;
        default:
            return MouseButton::Other;
    }
}

class WebWindowNative final
    : public LinuxWindowNative
    , public WebWindowSurface
{
public:
    WebWindowNative(const WindowOptions& options, WindowEvents& events, int id)
        : state(*this, options, events)
    {
        windowId = id;
        scale = webDevicePixelRatio();
        viewSurfaces = makeWebViewSurfaceBackend(*this);
        webWindows()[windowId] = this;

        // A page canvas with a layout of its own reports its size from inside
        // this call, which is why the window is registered first.
        canvasSelector = takeWebString(
            eacp_web_window_create(windowId, contentSize.x, contentSize.y));

        if (!canvasSelector.empty())
            nativeSurface = {NativeSurfaceHandle::Kind::Web,
                             nullptr,
                             (void*) canvasSelector.c_str(),
                             (uint32_t) windowId};

        updateBackingSize();
        eacp_web_window_set_title(windowId, state.title.c_str());
    }

    ~WebWindowNative() override
    {
        if (contentView != nullptr)
            linuxUnbindWindowFromContentView(*contentView);

        auto& seat = webSeat();

        if (seat.pointerWindow == this)
            seat.pointerWindow = nullptr;

        eacp_web_window_destroy(windowId);
        webWindows().erase(windowId);
    }

    LinuxWindowState& getState() override { return state; }

    void* getHandle() override
    {
        return canvasSelector.empty() ? nullptr : (void*) canvasSelector.c_str();
    }

    void setVisible(bool visible) override
    {
        if (visible == mapped)
            return;

        eacp_web_window_set_visible(windowId, visible ? 1 : 0);
        mapped = visible;

        if (visible)
        {
            state.notifyHostVisibility(true);
            surfacesChanged();
            return;
        }

        surfacesChanged();
        state.setActive(false);
        state.notifyHostVisibility(false);
    }

    void setSize(Point newSize) override
    {
        eacp_web_window_set_css_size(windowId, newSize.x, newSize.y);
        applySize(newSize);
    }

    void setTitle(const std::string& newTitle) override
    {
        state.title = newTitle;
        eacp_web_window_set_title(windowId, newTitle.c_str());
    }

    void minimize() override {}
    void toggleMaximize() override {}

    void setFullScreen(bool shouldBeFullScreen) override
    {
        state.fullScreenRequested = shouldBeFullScreen;
        eacp_web_window_set_full_screen(windowId, shouldBeFullScreen ? 1 : 0);
    }

    void setMouseLocked(bool locked) override
    {
        mouseLockIntent = locked;
        eacp_web_window_set_pointer_lock(windowId, locked ? 1 : 0);
    }

    bool isKeyPressed(uint16_t keyCode) override
    {
        return Keyboard::isKeyPressed(keyCode);
    }

    ModifierKeys getModifiers() override { return Keyboard::getModifiers(); }

    // The canvas's layout changed under it: the page's CSS, or full screen.
    void layoutResized(Point newSize)
    {
        if (newSize.x <= 0.f || newSize.y <= 0.f)
            return;

        applySize(newSize);
    }

    void scaleChanged(float newScale)
    {
        if (newScale <= 0.f || newScale == scale)
            return;

        scale = newScale;
        updateBackingSize();

        if (contentView == nullptr)
            return;

        // The surfaces take the new scale first, so the notification after
        // them reads it rather than the one it replaces.
        linuxWindowSurfaceStateChanged(*contentView);
        notifyBackingScaleChanged(*contentView);
    }

    void fullScreenChanged(bool isFullScreen)
    {
        state.fullScreen = isFullScreen;
        state.fullScreenRequested = isFullScreen;
    }

    void focusChanged(bool focused)
    {
        if (!focused)
            webKeyboardReset();

        state.setActive(focused);
    }

    void applyCursor(MouseCursor::Shape shape) const
    {
        eacp_web_window_set_cursor(windowId, webCursorName(shape));
    }

    void dispatchMouse(MouseEvent event) const
    {
        if (contentView == nullptr)
            return;

        event.downPos = event.type == MouseEventType::Wheel
                            ? event.pos
                            : webSeat().pointer.getDownPosition();

        contentView->dispatchMouseEvent(event);
    }

    bool dispatchKey(const KeyEvent& event) const
    {
        return contentView != nullptr && contentView->dispatchKeyEvent(event);
    }

private:
    void updateBackingSize(Point size)
    {
        pixelWidth = std::max(1, (int) std::lround(size.x * scale));
        pixelHeight = std::max(1, (int) std::lround(size.y * scale));

        eacp_web_window_set_backing_size(windowId, pixelWidth, pixelHeight);
    }

    void updateBackingSize() { updateBackingSize(contentSize); }

    // The backing size first, so the surfaces the resize syncs read the new one.
    void applySize(Point newSize)
    {
        updateBackingSize(newSize);
        state.resizeTo(newSize);
        surfacesChanged();
    }

    void surfacesChanged()
    {
        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);
    }

    LinuxWindowState state;
};

std::unique_ptr<LinuxWindowNative> makeWindowNative(const WindowOptions& options,
                                                    WindowEvents& events)
{
    if (!webHasDisplay())
        return makeHeadlessWindowNative(options, events);

    return std::make_unique<WebWindowNative>(options, events, nextWebWindowId());
}

WebWindowNative* findWebWindow(int id)
{
    auto& windows = webWindows();
    auto found = windows.find(id);

    return found == windows.end() ? nullptr : found->second;
}

void webRefreshCursor(bool force)
{
    auto& seat = webSeat();
    auto* window = seat.pointerWindow;

    if (window == nullptr || window->contentView == nullptr)
        return;

    auto* contentView = window->contentView;
    auto* hit = contentView->hitTest(seat.pointer.getPosition());
    auto shape = hit != nullptr ? hit->getMouseCursor().getShape()
                                : contentView->getMouseCursor().getShape();

    if (seat.cursor.setShape(shape) || force)
        window->applyCursor(shape);
}

MouseEvent webMouseEvent(MouseEventType type, int modifierMask, double timeMs)
{
    auto& seat = webSeat();

    auto event = MouseEvent {};
    event.type = type;
    event.pos = seat.pointer.getPosition();
    event.button = seat.pointer.getHeldButton();
    event.modifiers = webModifiersFromMask(modifierMask);
    event.clickCount = seat.pointer.getClickCount();
    event.timestamp = timeMs / 1000.0;
    return event;
}

void webPointerMoved(WebWindowNative& window,
                     Point position,
                     Point movement,
                     bool locked,
                     int modifierMask,
                     double timeMs,
                     bool fromTouch)
{
    auto& seat = webSeat();
    auto previous = seat.pointer.getPosition();
    auto delta =
        locked ? movement : Point {position.x - previous.x, position.y - previous.y};

    if (delta.x == 0.f && delta.y == 0.f && seat.pointerWindow == &window)
        return;

    seat.pointerWindow = &window;

    if (!locked)
        seat.pointer.setPosition(position);

    auto type = seat.pointer.isButtonHeld() ? MouseEventType::Dragged
                                            : MouseEventType::Moved;

    auto event = webMouseEvent(type, modifierMask, timeMs);
    event.delta = delta;
    event.rawDelta = movement;
    event.fromTouch = fromTouch;

    window.dispatchMouse(event);
    webRefreshCursor(false);
}

void webPointerButton(WebWindowNative& window,
                      Point position,
                      int button,
                      bool pressed,
                      int modifierMask,
                      double timeMs,
                      bool fromTouch)
{
    auto& seat = webSeat();
    seat.pointerWindow = &window;
    seat.pointer.setPosition(position);

    auto mouseButton = webButtonFromDom(button);

    if (pressed)
        seat.pointer.pressed(mouseButton, (uint32_t) timeMs);
    else
        seat.pointer.released();

    auto event = webMouseEvent(
        pressed ? MouseEventType::Down : MouseEventType::Up, modifierMask, timeMs);
    event.button = mouseButton;
    event.fromTouch = fromTouch;

    window.dispatchMouse(event);
}

void webPointerEntered(WebWindowNative& window, Point position)
{
    auto& seat = webSeat();
    seat.pointerWindow = &window;
    seat.pointer.setPosition(position);
    seat.cursor.setHidden(false);

    webRefreshCursor(true);
}

void webPointerLeft(WebWindowNative& window, int modifierMask, double timeMs)
{
    auto& seat = webSeat();

    if (seat.pointerWindow != &window)
        return;

    window.dispatchMouse(
        webMouseEvent(MouseEventType::Exited, modifierMask, timeMs));
    seat.pointerWindow = nullptr;
}

// DOM's wheel deltas grow downwards; MouseEvent::delta's do not. A pixel delta
// is a precise one already in points, lines and pages are notches.
void webWheel(WebWindowNative& window,
              Point position,
              Point domDelta,
              int deltaMode,
              int modifierMask,
              double timeMs)
{
    auto& seat = webSeat();
    seat.pointerWindow = &window;
    seat.pointer.setPosition(position);

    auto delta = Point {-domDelta.x, -domDelta.y};

    if (deltaMode == 0)
    {
        seat.wheel.setSource(true, false);
        seat.wheel.addDelta(delta);
    }
    else
    {
        auto lines = deltaMode == 2 ? webLinesPerPage : 1.f;

        seat.wheel.setSource(false, false);
        seat.wheel.addNotches({delta.x * lines, delta.y * lines});
    }

    auto event = webMouseEvent(MouseEventType::Wheel, modifierMask, timeMs);
    event.preciseScrolling = seat.wheel.isPrecise();
    event.delta = seat.wheel.getDelta();
    seat.wheel.endFrame();

    if (event.delta.x != 0.f || event.delta.y != 0.f)
        window.dispatchMouse(event);
}
} // namespace

bool webHasDisplay()
{
    return !Apps::getAppEnvironment().headless && eacp_web_window_has_dom() != 0;
}

float webDevicePixelRatio()
{
    auto ratio = (float) eacp_web_device_pixel_ratio();
    return ratio > 0.f ? ratio : 1.f;
}

LinuxWindowSurface* linuxPointerWindow()
{
    return webSeat().pointerWindow;
}

Point linuxPointerPosition()
{
    return webSeat().pointer.getPosition();
}

void linuxRefreshCursor()
{
    webRefreshCursor(false);
}

void linuxViewFocused(bool) {}

struct Window::Native
{
    Native(const WindowOptions& options, WindowEvents& events)
        : native(makeWindowNative(options, events))
    {
    }

    std::unique_ptr<LinuxWindowNative> native;
};

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(optionsToUse, events)
{
}

Window::~Window() = default;

void Window::setTitle(const std::string& title)
{
    impl->native->setTitle(title);
}

// The canvas's CSS selector as a const char*, null with no canvas.
void* Window::getHandle()
{
    return impl->native->getHandle();
}

void* Window::getContentViewHandle()
{
    return impl->native->getContentViewHandle();
}

void Window::setContentView(View& view)
{
    contentLink.attach(&view, this);
    impl->native->setContentView(&view);
}

// A canvas cannot raise itself above the page; it can only be shown.
void Window::toFront()
{
    impl->native->setVisible(true);
}

void Window::setVisible(bool visible)
{
    impl->native->setVisible(visible);
}

void Window::minimize()
{
    impl->native->minimize();
}

void Window::toggleMaximize()
{
    impl->native->toggleMaximize();
}

// Element.requestFullscreen, which a browser grants only from inside a user
// gesture: isFullScreen follows what it granted.
void Window::setFullScreen(bool shouldBeFullScreen)
{
    impl->native->setFullScreen(shouldBeFullScreen);
}

bool Window::isFullScreen() const
{
    return impl->native->isFullScreen();
}

bool Window::isVisible()
{
    return impl->native->isVisible();
}

Point Window::getPosition() const
{
    return impl->native->getPosition();
}

void Window::setPosition(Point position)
{
    impl->native->setPosition(position);
}

Point Window::getSize() const
{
    return impl->native->getWindowSurface().contentSize;
}

void Window::setSize(Point size)
{
    impl->native->setSize(options.effectiveSize(size));
}

void Window::setMouseLocked(bool locked)
{
    impl->native->setMouseLocked(locked);
}

bool Window::isMouseLocked() const
{
    return impl->native->isMouseLocked();
}

// The native unit here is the KeyCode itself.
bool Window::isKeyPressed(uint16_t keyCode) const
{
    return impl->native->isKeyPressed(keyCode);
}

bool Window::isShiftPressed() const
{
    return getModifiers().shift;
}

bool Window::isControlPressed() const
{
    return getModifiers().control;
}

bool Window::isAltPressed() const
{
    return getModifiers().alt;
}

bool Window::isCommandPressed() const
{
    return getModifiers().command;
}

ModifierKeys Window::getModifiers() const
{
    return impl->native->getModifiers();
}

} // namespace eacp::Graphics

extern "C"
{
    EMSCRIPTEN_KEEPALIVE void eacp_web_window_pointer(int id,
                                                      int type,
                                                      double x,
                                                      double y,
                                                      int button,
                                                      int modifiers,
                                                      double timeMs,
                                                      double movementX,
                                                      double movementY,
                                                      int locked,
                                                      int touch)
    {
        using namespace eacp::Graphics;
        auto* window = findWebWindow(id);

        if (window == nullptr)
            return;

        auto position = Point {(float) x, (float) y};
        auto movement = Point {(float) movementX, (float) movementY};

        switch ((WebPointerEvent) type)
        {
            case WebPointerEvent::Entered:
                webPointerEntered(*window, position);
                break;

            case WebPointerEvent::Left:
                webPointerLeft(*window, modifiers, timeMs);
                break;

            case WebPointerEvent::Moved:
                webPointerMoved(*window,
                                position,
                                movement,
                                locked != 0,
                                modifiers,
                                timeMs,
                                touch != 0);
                break;

            case WebPointerEvent::Pressed:
            case WebPointerEvent::Released:
                webPointerButton(*window,
                                 position,
                                 button,
                                 type == (int) WebPointerEvent::Pressed,
                                 modifiers,
                                 timeMs,
                                 touch != 0);
                break;
        }
    }

    EMSCRIPTEN_KEEPALIVE void eacp_web_window_wheel(int id,
                                                    double x,
                                                    double y,
                                                    double deltaX,
                                                    double deltaY,
                                                    int deltaMode,
                                                    int modifiers,
                                                    double timeMs)
    {
        using namespace eacp::Graphics;

        if (auto* window = findWebWindow(id))
            webWheel(*window,
                     {(float) x, (float) y},
                     {(float) deltaX, (float) deltaY},
                     deltaMode,
                     modifiers,
                     timeMs);
    }

    // Whether a view kept the key, which is whether the page's default action
    // for it is prevented.
    EMSCRIPTEN_KEEPALIVE int eacp_web_window_key(int id,
                                                 int down,
                                                 const char* code,
                                                 const char* key,
                                                 int modifiers,
                                                 int repeat,
                                                 double timeMs)
    {
        using namespace eacp::Graphics;

        auto event =
            webKeyEvent(down != 0, code, key, modifiers, repeat != 0, timeMs);
        webKeyboardEvent(event);

        auto* window = findWebWindow(id);

        return window != nullptr && window->dispatchKey(event) ? 1 : 0;
    }

    EMSCRIPTEN_KEEPALIVE void eacp_web_window_focus(int id, int focused)
    {
        if (auto* window = eacp::Graphics::findWebWindow(id))
            window->focusChanged(focused != 0);
    }

    EMSCRIPTEN_KEEPALIVE void
        eacp_web_window_resized(int id, double width, double height)
    {
        if (auto* window = eacp::Graphics::findWebWindow(id))
            window->layoutResized({(float) width, (float) height});
    }

    EMSCRIPTEN_KEEPALIVE void eacp_web_window_full_screen(int id, int fullScreen)
    {
        if (auto* window = eacp::Graphics::findWebWindow(id))
            window->fullScreenChanged(fullScreen != 0);
    }

    EMSCRIPTEN_KEEPALIVE void eacp_web_window_scale_changed(double ratio)
    {
        using namespace eacp::Graphics;

        auto windows = eacp::Vector<WebWindowNative*> {};

        for (auto& entry: webWindows())
            windows.add(entry.second);

        for (auto* window: windows)
            window->scaleChanged((float) ratio);
    }
}
