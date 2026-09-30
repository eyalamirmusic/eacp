#include "Window.h"

#include "../Graphics/Keyboard-Web.h"
#include "../View/WebViewSurface-Web.h"
#include "LinuxWindowSystem-Linux.h"

#include <eacp/Core/Threads/EventLoop.h>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <cmath>
#include <string_view>

// The page's one canvas is the window: the first Window takes it, and its
// content view fills it. Sizes are the canvas's CSS size, which is points; the
// drawing buffer is that times devicePixelRatio. Input is the DOM's, through
// html5.h: mouse and wheel on the canvas (moves and releases on the document,
// so a drag keeps going outside it), touch on the canvas, keys on the window.

namespace eacp::Graphics
{
namespace
{
constexpr auto webCanvasSelector = "#canvas";

// A mouse-down this close in time and place to the last one adds a click.
constexpr auto webMultiClickSeconds = 0.5;
constexpr auto webMultiClickSlop = 4.f;

// The safe area as CSS reports it, in CSS pixels: env() is only readable
// through a style, so a hidden element carries it. 0 top, 1 right, 2 bottom,
// 3 left.
EM_JS(double, webSafeAreaInset, (int side), {
    var probe = Module.eacpSafeAreaProbe;

    if (!probe)
    {
        probe = document.createElement('div');
        probe.style.cssText = 'position:fixed;left:0;top:0;width:0;height:0;'
                              + 'visibility:hidden;pointer-events:none;'
                              + 'padding-top:env(safe-area-inset-top);'
                              + 'padding-right:env(safe-area-inset-right);'
                              + 'padding-bottom:env(safe-area-inset-bottom);'
                              + 'padding-left:env(safe-area-inset-left)';
        document.body.appendChild(probe);
        Module.eacpSafeAreaProbe = probe;
    }

    var style = getComputedStyle(probe);
    var sides = [ 'paddingTop', 'paddingRight', 'paddingBottom', 'paddingLeft' ];
    return parseFloat(style[sides[side]]) || 0;
});

// The canvas's top left in the viewport: 0 x, 1 y.
EM_JS(double, webCanvasOrigin, (const char* selector, int axis), {
    var canvas = document.querySelector(UTF8ToString(selector));

    if (!canvas)
        return 0;

    var rect = canvas.getBoundingClientRect();
    return axis == 0 ? rect.left : rect.top;
});

EM_JS(void, webSetCanvasCursor, (const char* selector, const char* cursor), {
    var canvas = document.querySelector(UTF8ToString(selector));

    if (canvas)
        canvas.style.cursor = UTF8ToString(cursor);
});

EM_JS(bool, webHasCanvas, (const char* selector), {
    return document.querySelector(UTF8ToString(selector)) != null;
});

const char* webCursorName(MouseCursor cursor)
{
    switch (cursor)
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

MouseButton webMouseButton(unsigned short button)
{
    switch (button)
    {
        case 1:
            return MouseButton::Middle;
        case 2:
            return MouseButton::Right;
        default:
            return MouseButton::Left;
    }
}

ModifierKeys webModifiers(bool shift, bool control, bool alt, bool meta)
{
    return {shift, control, alt, meta};
}

ModifierKeys webModifiers(const EmscriptenMouseEvent& event)
{
    return webModifiers(event.shiftKey, event.ctrlKey, event.altKey, event.metaKey);
}

// The text a key typed: `key` when it is one character, since the named keys
// ("Enter", "ArrowLeft") are words.
std::string webTypedText(const char* key)
{
    auto text = std::string_view {key};
    auto codePoints = 0;

    for (auto byte: text)
        if (((unsigned char) byte & 0xC0) != 0x80)
            ++codePoints;

    return codePoints == 1 ? std::string {text} : std::string {};
}

struct WebWindow;

WebWindow*& webActiveWindow()
{
    static auto* window = static_cast<WebWindow*>(nullptr);
    return window;
}

bool webResized(int, const EmscriptenUiEvent*, void* data);
bool webMouseDown(int, const EmscriptenMouseEvent* event, void* data);
bool webMouseMoved(int, const EmscriptenMouseEvent* event, void* data);
bool webMouseUp(int, const EmscriptenMouseEvent* event, void* data);
bool webMouseEntered(int, const EmscriptenMouseEvent* event, void* data);
bool webMouseLeft(int, const EmscriptenMouseEvent* event, void* data);
bool webWheel(int, const EmscriptenWheelEvent* event, void* data);
bool webTouch(int type, const EmscriptenTouchEvent* event, void* data);
bool webKey(int type, const EmscriptenKeyboardEvent* event, void* data);
bool webFocus(int type, const EmscriptenFocusEvent* event, void* data);

struct WebWindow : WebWindowSurface
{
    WebWindow(const WindowOptions& optionsToUse, WindowEvents& eventsToUse)
        : onResize(optionsToUse.onResize)
        , events(&eventsToUse)
    {
        contentSize = optionsToUse.effectiveInitialSize();
        viewSurfaces = makeWebViewSurfaceBackend(*this);

        emscripten_set_window_title(optionsToUse.title.c_str());

        if (webActiveWindow() != nullptr || !webHasCanvas(webCanvasSelector))
            return;

        webActiveWindow() = this;
        nativeSurface = {NativeSurfaceHandle::Kind::Canvas,
                         nullptr,
                         nullptr,
                         0,
                         webCanvasSelector};
        mapped = true;

        listen(true);
        refresh();
    }

    ~WebWindow()
    {
        if (contentView != nullptr)
            linuxUnbindWindowFromContentView(*contentView);

        if (webActiveWindow() != this)
            return;

        listen(false);
        webActiveWindow() = nullptr;
    }

    void listen(bool on)
    {
        auto* data = on ? this : nullptr;
        auto* window = EMSCRIPTEN_EVENT_TARGET_WINDOW;
        auto* document = EMSCRIPTEN_EVENT_TARGET_DOCUMENT;
        auto* canvas = webCanvasSelector;

        emscripten_set_resize_callback(
            window, data, false, on ? webResized : nullptr);

        emscripten_set_mousedown_callback(
            canvas, data, false, on ? webMouseDown : nullptr);
        emscripten_set_mousemove_callback(
            document, data, false, on ? webMouseMoved : nullptr);
        emscripten_set_mouseup_callback(
            document, data, false, on ? webMouseUp : nullptr);
        emscripten_set_mouseenter_callback(
            canvas, data, false, on ? webMouseEntered : nullptr);
        emscripten_set_mouseleave_callback(
            canvas, data, false, on ? webMouseLeft : nullptr);
        emscripten_set_wheel_callback(canvas, data, false, on ? webWheel : nullptr);

        emscripten_set_touchstart_callback(
            canvas, data, false, on ? webTouch : nullptr);
        emscripten_set_touchmove_callback(
            canvas, data, false, on ? webTouch : nullptr);
        emscripten_set_touchend_callback(
            canvas, data, false, on ? webTouch : nullptr);
        emscripten_set_touchcancel_callback(
            canvas, data, false, on ? webTouch : nullptr);

        emscripten_set_keydown_callback(window, data, false, on ? webKey : nullptr);
        emscripten_set_keyup_callback(window, data, false, on ? webKey : nullptr);

        emscripten_set_focus_callback(window, data, false, on ? webFocus : nullptr);
        emscripten_set_blur_callback(window, data, false, on ? webFocus : nullptr);
    }

    // Reads the canvas's CSS size, the pixel ratio and the safe area afresh,
    // and sizes the drawing buffer to match. True when the size in points
    // changed.
    bool refresh()
    {
        auto oldSize = contentSize;
        auto cssWidth = 0.0;
        auto cssHeight = 0.0;

        emscripten_get_element_css_size(webCanvasSelector, &cssWidth, &cssHeight);

        scale = (float) emscripten_get_device_pixel_ratio();

        if (scale <= 0.f)
            scale = 1.f;

        pixelWidth = (int) std::lround(cssWidth * scale);
        pixelHeight = (int) std::lround(cssHeight * scale);
        emscripten_set_canvas_element_size(
            webCanvasSelector, pixelWidth, pixelHeight);

        contentSize = {(float) cssWidth, (float) cssHeight};

        insets.top = (float) webSafeAreaInset(0);
        insets.right = (float) webSafeAreaInset(1);
        insets.bottom = (float) webSafeAreaInset(2);
        insets.left = (float) webSafeAreaInset(3);

        if (contentView != nullptr)
            contentView->setSafeAreaInsets(insets);

        return contentSize.x != oldSize.x || contentSize.y != oldSize.y;
    }

    void resized()
    {
        auto oldScale = scale;
        auto sizeChanged = refresh();

        if (contentView == nullptr)
            return;

        if (sizeChanged)
            layOutContent();

        if (scale != oldScale)
            notifyBackingScaleChanged(*contentView);

        linuxWindowSurfaceStateChanged(*contentView);
    }

    void layOutContent()
    {
        if (contentView == nullptr)
            return;

        contentView->setBounds({0.f, 0.f, contentSize.x, contentSize.y});

        if (onResize)
            onResize((int) contentSize.x, (int) contentSize.y);
    }

    void setContentView(View* view)
    {
        if (contentView != nullptr)
            linuxUnbindWindowFromContentView(*contentView);

        contentView = view;

        if (contentView == nullptr)
            return;

        contentView->setSafeAreaInsets(insets);
        layOutContent();
        linuxBindWindowToContentView(*contentView, *this);

        // A native window reports its first size after its constructor has
        // returned, by when the app has added the content's subviews; the
        // canvas has its size already, so the content lays out again a turn
        // later for them.
        Threads::callAsync(
            [this, view]
            {
                if (webActiveWindow() == this && contentView == view)
                    layOutContent();
            });
    }

    Point canvasPoint(const EmscriptenMouseEvent& event) const
    {
        return {(float) (event.clientX - webCanvasOrigin(webCanvasSelector, 0)),
                (float) (event.clientY - webCanvasOrigin(webCanvasSelector, 1))};
    }

    MouseEvent mouseEvent(const EmscriptenMouseEvent& event, MouseEventType type)
    {
        auto result = MouseEvent {};
        result.type = type;
        result.pos = canvasPoint(event);
        result.downPos = downPos;
        result.delta = {(float) event.movementX, (float) event.movementY};
        result.rawDelta = result.delta;
        result.button = webMouseButton(event.button);
        result.modifiers = webModifiers(event);
        result.clickCount = clickCount;
        result.timestamp = event.timestamp / 1000.0;
        return result;
    }

    void dispatchMouse(const MouseEvent& event)
    {
        pointerPosition = event.pos;

        if (contentView != nullptr)
            contentView->dispatchMouseEvent(event);

        refreshCursor();
    }

    void mouseDown(const EmscriptenMouseEvent& event)
    {
        auto pos = canvasPoint(event);
        auto seconds = event.timestamp / 1000.0;

        auto near = std::abs(pos.x - downPos.x) <= webMultiClickSlop
                    && std::abs(pos.y - downPos.y) <= webMultiClickSlop;

        clickCount = near && seconds - lastDownSeconds <= webMultiClickSeconds
                         ? clickCount + 1
                         : 1;
        lastDownSeconds = seconds;
        downPos = pos;
        buttonsHeld |= 1u << event.button;

        dispatchMouse(mouseEvent(event, MouseEventType::Down));
    }

    // Moves outside the canvas count only while a button is held there.
    void mouseMoved(const EmscriptenMouseEvent& event)
    {
        if (buttonsHeld != 0)
            dispatchMouse(mouseEvent(event, MouseEventType::Dragged));
        else if (pointerInside)
            dispatchMouse(mouseEvent(event, MouseEventType::Moved));
    }

    void mouseUp(const EmscriptenMouseEvent& event)
    {
        auto bit = 1u << event.button;

        if ((buttonsHeld & bit) == 0)
            return;

        buttonsHeld &= ~bit;
        dispatchMouse(mouseEvent(event, MouseEventType::Up));
    }

    void mouseLeft(const EmscriptenMouseEvent& event)
    {
        pointerInside = false;

        if (buttonsHeld == 0 && contentView != nullptr)
            contentView->dispatchMouseEvent(
                mouseEvent(event, MouseEventType::Exited));
    }

    // Pixels are exact, lines are wheel notches, pages are a screenful.
    void wheel(const EmscriptenWheelEvent& event)
    {
        auto result = mouseEvent(event.mouse, MouseEventType::Wheel);
        auto unit = event.deltaMode == DOM_DELTA_PAGE ? contentSize.y : 1.f;

        result.downPos = result.pos;
        result.delta = {(float) -event.deltaX * unit, (float) -event.deltaY * unit};
        result.rawDelta = result.delta;
        result.preciseScrolling = event.deltaMode != DOM_DELTA_LINE;

        if (contentView != nullptr
            && (result.delta.x != 0.f || result.delta.y != 0.f))
            contentView->dispatchMouseEvent(result);
    }

    // Touch ids are the DOM's plus one, so a finger is never 0, as on iOS.
    void touch(int type, const EmscriptenTouchEvent& event)
    {
        if (contentView == nullptr)
            return;

        auto phase = TouchPhase::Moved;

        if (type == EMSCRIPTEN_EVENT_TOUCHSTART)
            phase = TouchPhase::Began;
        else if (type == EMSCRIPTEN_EVENT_TOUCHEND)
            phase = TouchPhase::Ended;
        else if (type == EMSCRIPTEN_EVENT_TOUCHCANCEL)
            phase = TouchPhase::Cancelled;

        for (auto i = 0; i < event.numTouches; ++i)
        {
            const auto& point = event.touches[i];

            if (!point.isChanged)
                continue;

            auto result = TouchEvent {};
            result.id = (int) point.identifier + 1;
            result.phase = phase;
            result.pos = {(float) point.targetX, (float) point.targetY};
            result.timestamp = event.timestamp / 1000.0;

            contentView->dispatchTouchEvent(result);
        }
    }

    // A key the page should also act on goes on to the browser: its own
    // shortcuts (Ctrl or Cmd held) and the function keys.
    bool key(int type, const EmscriptenKeyboardEvent& event)
    {
        auto keyCode = webKeyCodeFromCode(event.code);
        auto down = type == EMSCRIPTEN_EVENT_KEYDOWN;
        auto modifiers =
            webModifiers(event.shiftKey, event.ctrlKey, event.altKey, event.metaKey);

        webModifiersChanged(modifiers);
        webKeyChanged(keyCode, down);

        if (contentView == nullptr)
            return false;

        auto result = KeyEvent {};
        result.keyCode = keyCode;
        result.type = down ? KeyEventType::Down : KeyEventType::Up;
        result.modifiers = modifiers;
        result.isRepeat = event.repeat;
        result.timestamp = event.timestamp / 1000.0;
        result.characters = webTypedText(event.key);
        result.charactersIgnoringModifiers = Keyboard::keyCodeToCharacter(keyCode);

        if (result.charactersIgnoringModifiers.empty())
            result.charactersIgnoringModifiers = result.characters;

        if (down)
            contentView->keyDown(result);
        else
            contentView->keyUp(result);

        auto functionKey = std::string_view {event.code}.starts_with("F")
                           && std::string_view {event.code}.size() > 1;

        return !(modifiers.control || modifiers.command || functionKey);
    }

    void focusChanged(bool focused)
    {
        if (!focused)
        {
            webReleaseAllKeys();
            buttonsHeld = 0;
        }

        events->onActivationChanged(focused);
    }

    void refreshCursor()
    {
        if (contentView == nullptr)
            return;

        auto* hit = contentView->hitTest(pointerPosition);
        auto cursor =
            hit != nullptr ? hit->getMouseCursor() : contentView->getMouseCursor();

        webSetCanvasCursor(webCanvasSelector, webCursorName(cursor));
    }

    ResizeCallback onResize;
    WindowEvents* events;

    Insets insets;

    Point pointerPosition;
    Point downPos;
    unsigned buttonsHeld = 0;
    int clickCount = 1;
    double lastDownSeconds = -1.0;
    bool pointerInside = false;
};

WebWindow& webWindowFrom(void* data)
{
    return *static_cast<WebWindow*>(data);
}

bool webResized(int, const EmscriptenUiEvent*, void* data)
{
    webWindowFrom(data).resized();
    return false;
}

bool webMouseDown(int, const EmscriptenMouseEvent* event, void* data)
{
    webWindowFrom(data).mouseDown(*event);
    return true;
}

bool webMouseMoved(int, const EmscriptenMouseEvent* event, void* data)
{
    webWindowFrom(data).mouseMoved(*event);
    return false;
}

bool webMouseUp(int, const EmscriptenMouseEvent* event, void* data)
{
    webWindowFrom(data).mouseUp(*event);
    return false;
}

bool webMouseEntered(int, const EmscriptenMouseEvent*, void* data)
{
    webWindowFrom(data).pointerInside = true;
    return false;
}

bool webMouseLeft(int, const EmscriptenMouseEvent* event, void* data)
{
    webWindowFrom(data).mouseLeft(*event);
    return false;
}

// Handled, so the page itself does not scroll or zoom.
bool webWheel(int, const EmscriptenWheelEvent* event, void* data)
{
    webWindowFrom(data).wheel(*event);
    return true;
}

// Handled, so the browser neither scrolls nor makes mouse events of them.
bool webTouch(int type, const EmscriptenTouchEvent* event, void* data)
{
    webWindowFrom(data).touch(type, *event);
    return true;
}

bool webKey(int type, const EmscriptenKeyboardEvent* event, void* data)
{
    return webWindowFrom(data).key(type, *event);
}

bool webFocus(int type, const EmscriptenFocusEvent*, void* data)
{
    webWindowFrom(data).focusChanged(type == EMSCRIPTEN_EVENT_FOCUS);
    return false;
}
} // namespace

struct Window::Native : WebWindow
{
    using WebWindow::WebWindow;
};

LinuxWindowSurface* linuxPointerWindow()
{
    auto* window = webActiveWindow();
    return window != nullptr && window->pointerInside ? window : nullptr;
}

Point linuxPointerPosition()
{
    auto* window = webActiveWindow();
    return window != nullptr ? window->pointerPosition : Point {};
}

void linuxRefreshCursor()
{
    if (auto* window = webActiveWindow())
        window->refreshCursor();
}

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(optionsToUse, events)
{
}

Window::~Window() = default;

void Window::setTitle(const std::string& title)
{
    emscripten_set_window_title(title.c_str());
}

void* Window::getHandle()
{
    return impl.get();
}

void* Window::getContentViewHandle()
{
    return impl->contentView != nullptr ? impl->contentView->getHandle() : nullptr;
}

void Window::setContentView(View& view)
{
    contentLink.attach(&view, this);
    impl->setContentView(&view);
}

// The page decides where the canvas is and how big: nothing to raise, hide,
// move or size.
void Window::toFront() {}

void Window::setVisible(bool) {}

bool Window::isVisible()
{
    return impl->mapped;
}

Point Window::getPosition() const
{
    return {};
}

void Window::setPosition(Point) {}

Point Window::getSize() const
{
    return impl->contentSize;
}

void Window::setSize(Point) {}

void Window::minimize() {}

void Window::toggleMaximize() {}

// The browser grants a lock only inside a user gesture, so an early request is
// deferred to the next click on the canvas.
void Window::setMouseLocked(bool locked)
{
    if (locked)
        emscripten_request_pointerlock(webCanvasSelector, true);
    else
        emscripten_exit_pointerlock();
}

bool Window::isMouseLocked() const
{
    auto status = EmscriptenPointerlockChangeEvent {};

    return emscripten_get_pointerlock_status(&status) == EMSCRIPTEN_RESULT_SUCCESS
           && status.isActive;
}

bool Window::isKeyPressed(uint16_t virtualKeyCode) const
{
    return Keyboard::isKeyPressed(virtualKeyCode);
}

bool Window::isShiftPressed() const
{
    return Keyboard::isShiftPressed();
}

bool Window::isControlPressed() const
{
    return Keyboard::isControlPressed();
}

bool Window::isAltPressed() const
{
    return Keyboard::isAltPressed();
}

bool Window::isCommandPressed() const
{
    return Keyboard::isCommandPressed();
}

ModifierKeys Window::getModifiers() const
{
    return Keyboard::getModifiers();
}
} // namespace eacp::Graphics
