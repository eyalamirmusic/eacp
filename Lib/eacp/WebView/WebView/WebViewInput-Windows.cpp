#include "WebView-Windows.h"

#include "WebViewMessages.h"
#include "FileDrag-Windows.h"
#include <eacp/Graphics/View/FileDragOut-Windows.h>

#include <shlobj.h>
#include <ole2.h>

namespace eacp::Graphics
{

namespace
{
// The button held during a drag. WebView2 treats a MOVE whose virtualKeys
// report no button as a plain hover, so without this a scrollbar-thumb (or
// any in-page) drag is dropped the moment the pointer moves.
COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS
heldButtonFor(const MouseEvent& event)
{
    if (event.type != MouseEventType::Dragged)
        return COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
    if (event.button == MouseButton::Right)
        return COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
    if (event.button == MouseButton::Middle)
        return COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
    return COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
}

COREWEBVIEW2_MOUSE_EVENT_KIND downKindFor(MouseButton button)
{
    if (button == MouseButton::Right)
        return COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
    if (button == MouseButton::Middle)
        return COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
    return COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
}

COREWEBVIEW2_MOUSE_EVENT_KIND upKindFor(MouseButton button)
{
    if (button == MouseButton::Right)
        return COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
    if (button == MouseButton::Middle)
        return COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
    return COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
}

// Re-injects an unconsumed key into the host window's normal keyboard path
// (CompositionHostWindow turns it back into a framework KeyEvent). Always a
// plain key message -- never WM_SYSKEYDOWN, whose DefWindowProc would poke the
// window menu. Bit 30 of lParam marks an auto-repeat keydown, the one field the
// host actually reads back.
constexpr auto keyRepeatFlag = LPARAM {0x40000000};

void forwardKeyToHost(HWND host, const detail::KeyVerdict& verdict)
{
    if (!host || verdict.virtualKey == 0)
        return;

    auto message = verdict.isDown ? UINT {WM_KEYDOWN} : UINT {WM_KEYUP};
    auto lParam = LPARAM {verdict.isDown && verdict.isRepeat ? keyRepeatFlag : 0};

    PostMessageW(host, message, static_cast<WPARAM>(verdict.virtualKey), lParam);
}

void runFileDragLoop(HWND hostWindow,
                     float dpiScale,
                     IDataObject* dataObject,
                     const decltype(WebView::onFileDragMoved)& onDragMoved,
                     const decltype(WebView::onFileDragEnded)& onDragEnded)
{
    // Attach (not construct-from-raw) so the source keeps the
    // single ref it is born with; the ComPtr releases it — and
    // deletes the object — when the drag returns.
    ComPtr<IDropSource> dropSource;
    dropSource.Attach(
        new FileDragSource([hostWindow, dpiScale]
                           { return fileDragPointFromCursor(hostWindow, dpiScale); },
                           onDragMoved));

    // Register our own window as a drop target for the length
    // of the drag purely for cursor feedback (see
    // FileDragTarget) — without it the shell shows ⊘ over our
    // own window. Fails harmlessly (leaving the old ⊘) if
    // something else already owns the window's drop slot.
    ComPtr<IDropTarget> dropTarget;
    dropTarget.Attach(new FileDragTarget());
    auto registered = SUCCEEDED(RegisterDragDrop(hostWindow, dropTarget.Get()));

    DWORD effect = DROPEFFECT_NONE;
    SHDoDragDrop(hostWindow, dataObject, dropSource.Get(), DROPEFFECT_COPY, &effect);

    if (registered)
        RevokeDragDrop(hostWindow);

    // The modal loop is over: report the release point so the
    // app can file the drop (or stand its filing UI down).
    onDragEnded(fileDragPointFromCursor(hostWindow, dpiScale));
}
} // namespace

// --- Input forwarding ---------------------------------------------------
// A visual-hosted WebView2 has no input HWND, so the framework forwards the
// mouse events it routed to this View (see the WebView::mouse* overrides).
void WebView::Native::sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND kind,
                                const Point& localPos,
                                uint32_t mouseData,
                                COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS virtualKeys)
{
    if (!compositionController)
        return;

    // Bounds are in physical pixels (see updateBounds), so input points must
    // be too. The framework hands us positions local to this View in logical
    // units, so scale them up by the DPI factor.
    auto scale = getDpiScale();
    POINT pt = {static_cast<LONG>(std::lround(localPos.x * scale)),
                static_cast<LONG>(std::lround(localPos.y * scale))};

    compositionController->SendMouseInput(kind, virtualKeys, mouseData, pt);
}

void WebView::Native::handleMouseDown(const MouseEvent& event)
{
    resetDragArming();

    if (controller)
        controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);

    sendMouse(downKindFor(event.button), event.pos);
}

void WebView::Native::handleMouseUp(const MouseEvent& event)
{
    sendMouse(upKindFor(event.button), event.pos);
}

void WebView::Native::moveFocusToContent()
{
    if (hostHwnd)
        SetFocus(hostHwnd);

    if (controller)
        controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void WebView::Native::handleMouseMove(const MouseEvent& event)
{
    sendMouse(
        COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE, event.pos, 0, heldButtonFor(event));
}

void WebView::Native::handleMouseWheel(const MouseEvent& event)
{
    // event.delta is in lines; WebView2 expects WHEEL_DELTA units packed
    // into mouseData. y drives the vertical wheel, x the horizontal.
    auto toWheelDelta = [](float lines)
    {
        return static_cast<uint32_t>(
            static_cast<int32_t>(lines * static_cast<float>(WHEEL_DELTA)));
    };

    if (event.delta.y != 0.f)
        sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL,
                  event.pos,
                  toWheelDelta(event.delta.y));

    if (event.delta.x != 0.f)
        sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL,
                  event.pos,
                  toWheelDelta(event.delta.x));
}

void WebView::Native::handleMouseLeave()
{
    sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, {0.0f, 0.0f});
}

// --- Native file drag-out -----------------------------------------------
// The page arms a drag on mousedown (via the armFileDrag bridge command);
// by the time the pointer starts dragging the armed paths have arrived over
// the message channel, so the first mouseDragged after arming kicks off the
// OS drag. Mirrors the macOS EacpDragWebView gesture.
bool WebView::Native::startArmedFileDragIfNeeded()
{
    if (!dragArmed)
        return false;

    dragArmed = false;
    auto paths = std::move(armedDragPaths);
    armedDragPaths = {};
    performFileDrag(paths);
    return true;
}

void WebView::Native::performFileDrag(const Vector<std::string>& paths)
{
    if (paths.empty() || !hostHwnd)
        return;

    auto hostWindow = hostHwnd;
    auto dpiScale = getDpiScale();
    auto onDragStarted = owner.onFileDragStarted;
    auto onDragMoved = owner.onFileDragMoved;
    auto onDragEnded = owner.onFileDragEnded;
    auto oleHr = OleInitialize(nullptr);
    if (FAILED(oleHr))
        return;

    auto dataObject = createFileDataObject(paths);

    if (dataObject)
    {
        if (onDragStarted)
            onDragStarted();

        if (IsWindow(hostWindow))
            runFileDragLoop(
                hostWindow, dpiScale, dataObject.Get(), onDragMoved, onDragEnded);
    }

    dataObject.Reset();

    if (SUCCEEDED(oleHr))
        OleUninitialize();
}

// --- Native window drag -------------------------------------------------
// window-drag.js posts __eacpWindowDrag on a drag-region mousedown, which
// arms this; the next mouseDragged hands the gesture to the OS. Mirrors the
// macOS performWindowDragWithEvent: path.
bool WebView::Native::startArmedWindowDragIfNeeded()
{
    if (!windowDragArmed)
        return false;

    windowDragArmed = false;
    performWindowDrag();
    return true;
}

void WebView::Native::performWindowDrag()
{
    if (!hostHwnd)
        return;

    // The canonical borderless-window drag: drop the capture the host has
    // from the button press, then tell the window the user grabbed its
    // caption. DefWindowProc then runs a modal move loop that follows the
    // still-down mouse until release.
    ReleaseCapture();
    SendMessageW(hostHwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

// Clear any arming left from a previous gesture, before the page sees this
// mousedown and (asynchronously) re-arms for the new one. Without this a
// click that armed but never dragged would leak its arm into a later drag.
void WebView::Native::resetDragArming()
{
    dragArmed = false;
    armedDragPaths = {};
    windowDragArmed = false;
}

// Visual hosting gives the WebView no input HWND of its own, and the key-focus
// routing that needs this is macOS-only, so the host view is the right target.
void* WebView::nativeFocusTarget()
{
    return View::nativeFocusTarget();
}

// In visual hosting mode the WebView is a composition visual with no input
// HWND, so the framework's routed mouse events are forwarded to the WebView2
// composition controller. (On macOS the native WKWebView receives input
// directly, so the equivalents there are no-ops.)
void WebView::mouseDown(const MouseEvent& event)
{
    impl->handleMouseDown(event);
}

void WebView::mouseUp(const MouseEvent& event)
{
    impl->handleMouseUp(event);
}

void WebView::mouseDragged(const MouseEvent& event)
{
    // A drag armed by the page takes over the gesture: starting it here, from
    // the genuine drag, is what lets a file drag escape to Explorer / a window
    // drag enter the OS move loop.
    if (impl->startArmedFileDragIfNeeded())
        return;

    if (impl->startArmedWindowDragIfNeeded())
        return;

    impl->handleMouseMove(event);
}

void WebView::mouseMoved(const MouseEvent& event)
{
    impl->handleMouseMove(event);
}

void WebView::mouseExited(const MouseEvent&)
{
    impl->handleMouseLeave();
}

void WebView::mouseWheel(const MouseEvent& event)
{
    impl->handleMouseWheel(event);
}

void WebView::armFileDrag(const Vector<std::string>& paths)
{
    // Defer the actual OS drag to the next mouseDragged (see
    // startArmedFileDragIfNeeded): a drag started straight from this async
    // bridge callback wouldn't be tied to the live mouse gesture.
    impl->armedDragPaths = paths;
    impl->dragArmed = true;
}

void WebView::armWindowDrag()
{
    // Defer to the next mouseDragged (see startArmedWindowDragIfNeeded): a move
    // loop started straight from this async bridge callback wouldn't be tied to
    // the live mouse gesture, and a mere click would start dragging the window.
    impl->windowDragArmed = true;
}

void WebView::performWindowControl(const std::string& action)
{
    auto root = impl->hostHwnd ? GetAncestor(impl->hostHwnd, GA_ROOT) : nullptr;
    if (!root)
        return;

    if (action == "minimize")
    {
        ShowWindow(root, SW_MINIMIZE);
        return;
    }

    if (action == "maximize")
    {
        ShowWindow(root, IsZoomed(root) ? SW_RESTORE : SW_MAXIMIZE);

        // Report the resulting state back so the page's data-eacp-maximized
        // attribute tracks reality instead of guessing.
        evaluateJavaScript(detail::buildMaximizedStateScript(IsZoomed(root)));
        return;
    }

    if (action == "close")
        PostMessageW(root, WM_CLOSE, 0, 0);
}

// Windows counterpart to the macOS installKeyEventSupport (see WebView.mm).
// WebView2 exposes no native hook for plain character keys -- its
// AcceleratorKeyPressed event only fires for Ctrl/Alt combos and non-character
// keys, never a bare Space -- so we make the injected key-events.js the single
// source of truth: it reports every key the page saw, whether the page consumed
// it, and the key's identity. Unconsumed keys go to onUnhandledKeyEvent and,
// unless it claims them, are re-injected into the host window's WM_KEYDOWN path
// -- the Windows analog of macOS's walk up the responder chain past the
// framework container.
void WebView::installKeyEventSupport()
{
    auto shim = ResEmbed::get("key-events.js", "EacpWebView");
    if (!shim)
        throw std::runtime_error(
            "eacp-webview: embedded key-events.js resource not found");

    addUserScript(shim.toString(), true);

    addScriptMessageHandler(
        "__eacpKeyEvent",
        [this](const std::string& message)
        {
            auto parsed = detail::parseKeyVerdict(message);
            if (!parsed || parsed->consumed)
                return;

            const auto& verdict = *parsed;

            auto event = KeyEvent {};
            event.type = verdict.isDown ? KeyEventType::Down : KeyEventType::Up;
            event.keyCode = keyCodeFromVirtualKey(verdict.virtualKey);
            event.modifiers = verdict.modifiers;
            event.isRepeat = verdict.isRepeat;
            event.characters = verdict.characters;
            event.charactersIgnoringModifiers = verdict.characters;

            if (onUnhandledKeyEvent && onUnhandledKeyEvent(event))
                return;

            forwardKeyToHost(impl->hostHwnd, verdict);
        });
}

} // namespace eacp::Graphics
