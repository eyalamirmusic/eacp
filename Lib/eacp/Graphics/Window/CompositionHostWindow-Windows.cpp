#include "CompositionHostWindow-Windows.h"
#include "../Helpers/StringUtils-Windows.h"
#include "../Layers/NativeLayer-Windows.h"

#include <unordered_map>

namespace eacp::Graphics
{

// paintDirtyViewsForHost() is defined in View-Windows.cpp; the composition
// device accessors come from DComp-Windows.h. Both are linked earlier in the
// unity build.
void paintDirtyViewsForHost(HWND host);
void redrawAllCompositionHosts();

// Defined in Keyboard-Windows.cpp: Windows virtual key -> framework KeyCode
// (KeyCode::Unknown when unmapped), so KeyEvent::keyCode means the same thing
// on every platform.
uint16_t keyCodeFromVirtualKey(int vk);

namespace
{
std::unordered_map<View*, HWND>& contentViewToHwnd()
{
    static auto map = std::unordered_map<View*, HWND>();
    return map;
}

// Virtual key codes - defined manually to reduce Windows.h dependency.
namespace VK
{
constexpr uint16_t Shift = 0x10;
constexpr uint16_t Control = 0x11;
constexpr uint16_t Menu = 0x12; // Alt key
constexpr uint16_t LWin = 0x5B;
constexpr uint16_t RWin = 0x5C;
} // namespace VK

int getXFromLParam(LPARAM lp)
{
    return static_cast<int>(static_cast<short>(LOWORD(lp)));
}

int getYFromLParam(LPARAM lp)
{
    return static_cast<int>(static_cast<short>(HIWORD(lp)));
}

MouseButton buttonForDown(UINT msg)
{
    return msg == WM_LBUTTONDOWN   ? MouseButton::Left
           : msg == WM_RBUTTONDOWN ? MouseButton::Right
                                   : MouseButton::Middle;
}

MouseButton buttonForUp(UINT msg)
{
    return msg == WM_LBUTTONUP   ? MouseButton::Left
           : msg == WM_RBUTTONUP ? MouseButton::Right
                                 : MouseButton::Middle;
}

MouseEvent makeMouseEvent(LPARAM lParam,
                          float scale,
                          MouseEventType type,
                          const ModifierKeys& modifiers)
{
    MouseEvent event;
    event.pos = {static_cast<float>(getXFromLParam(lParam)) / scale,
                 static_cast<float>(getYFromLParam(lParam)) / scale};
    event.type = type;
    event.modifiers = modifiers;
    return event;
}
} // namespace

void registerContentViewHwnd(View* root, HWND hwnd)
{
    contentViewToHwnd()[root] = hwnd;
}

void unregisterContentViewHwnd(View* root)
{
    contentViewToHwnd().erase(root);
}

HWND findHostHwndForView(View* view)
{
    auto* root = view;
    while (root && root->getParent())
        root = root->getParent();

    if (!root)
        return nullptr;

    auto& map = contentViewToHwnd();
    auto it = map.find(root);
    return it == map.end() ? nullptr : it->second;
}

void repaintViewTree(View* view)
{
    if (!view)
        return;

    view->repaint();

    for (auto* subview: view->getSubviews())
        repaintViewTree(subview);
}

namespace
{
void markViewTreeLayersDirty(const View* view)
{
    if (!view)
        return;

    for (auto* layer: view->getLayers())
        if (auto* native = static_cast<NativeLayerBase*>(layer->getNativeLayer()))
            native->markContentDirty();

    for (auto* subview: view->getSubviews())
        markViewTreeLayersDirty(subview);
}

// Every live host, so a device loss can rebuild each one's target and root.
// Main-thread only, like the view/HWND registry above.
Vector<CompositionHostWindow*>& compositionHosts()
{
    static auto hosts = Vector<CompositionHostWindow*>();
    return hosts;
}
} // namespace

// Called by the rendering-device recovery in D2DFactory-Windows.cpp. Unlike
// Windows.UI.Composition — which could hot-swap the rendering device and keep
// its surfaces (they merely lost their pixels) — DirectComposition binds the
// device at creation, so the target, the root visual and every layer/view visual
// below them are dead objects. Rebuild the hosts' targets first, then let the
// generation stamp on each view/layer pull the rest through on the redraw.
void rebuildAllCompositionHosts()
{
    for (auto* host: compositionHosts())
    {
        host->initializeComposition(host->topMostTarget);

        if (host->contentView)
            host->attachContentView(host->contentView);
    }

    redrawAllCompositionHosts();
}

// Re-renders every layer and repaints every painting view of all
// composition-hosted windows.
void redrawAllCompositionHosts()
{
    for (auto& [root, hostHwnd]: contentViewToHwnd())
    {
        markViewTreeLayersDirty(root);
        repaintViewTree(root);
        InvalidateRect(hostHwnd, nullptr, FALSE);
    }
}

void CompositionHostWindow::initializeComposition(bool topMost)
{
    if (!hwnd)
        return;

    topMostTarget = topMost;

    auto* device = getCompositionDevice();
    if (!device)
        return;

    EA::Vectors::addIfNotThere(compositionHosts(), this);

    // Drop anything built against a previous device before rebuilding.
    rootVisual.Reset();
    target.Reset();
    generation = getCompositionGeneration();

    if (FAILED(device->CreateTargetForHwnd(hwnd, topMost, target.GetAddressOf())))
    {
        target.Reset();
        return;
    }

    if (FAILED(device->CreateVisual(rootVisual.GetAddressOf())))
    {
        rootVisual.Reset();
        target.Reset();
        return;
    }

    rescaleRootVisualToDpi();
    target->SetRoot(rootVisual.Get());
    commitComposition();
}

float CompositionHostWindow::getDpiScale() const
{
    auto dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return static_cast<float>(dpi) / 96.f;
}

// The root maps logical points to physical pixels for the whole tree. Content
// visuals below it counter-scale by 1/dpiScale so their physical-pixel surfaces
// land 1:1 — see the note in NativeLayer-Windows.h.
void CompositionHostWindow::rescaleRootVisualToDpi()
{
    if (!rootVisual)
        return;

    auto scale = getDpiScale();
    rootVisual->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    commitComposition();
}

void CompositionHostWindow::attachContentView(View* view)
{
    if (contentView && contentView != view)
        unregisterContentViewHwnd(contentView);

    contentView = view;

    if (!hwnd || !view)
        return;

    registerContentViewHwnd(view, hwnd);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    auto scale = getDpiScale();
    view->setBounds({0.f,
                     0.f,
                     static_cast<float>(clientRect.right) / scale,
                     static_cast<float>(clientRect.bottom) / scale});

    auto* viewVisual = static_cast<IDCompositionVisual2*>(view->getHandle());

    if (rootVisual && viewVisual)
        insertVisualAtTop(rootVisual.Get(), viewVisual);

    ensureAllLayersRendered(view);
}

void CompositionHostWindow::ensureAllLayersRendered(const View* view) const
{
    ensureAllLayersRendered(view, getDpiScale());

    // DComp batches every visual and surface change made above; nothing reaches
    // the screen until the device is flushed. WinRT committed implicitly, so
    // this is the one call the old backend never needed.
    commitComposition();
}

void CompositionHostWindow::ensureAllLayersRendered(const View* view,
                                                    float dpiScale) const
{
    if (!view)
        return;

    for (auto* layer: view->getLayers())
    {
        auto* native = static_cast<NativeLayerBase*>(layer->getNativeLayer());
        if (native)
        {
            native->setDpiScale(dpiScale);
            native->ensureContent();
        }
    }

    for (auto* subview: view->getSubviews())
        ensureAllLayersRendered(subview, dpiScale);
}

bool CompositionHostWindow::isKeyPressed(uint16_t vk) const
{
    return vk < 256 && keyState.test(vk);
}

bool CompositionHostWindow::isShiftPressed() const
{
    return isKeyPressed(VK::Shift);
}

bool CompositionHostWindow::isControlPressed() const
{
    return isKeyPressed(VK::Control);
}

bool CompositionHostWindow::isAltPressed() const
{
    return isKeyPressed(VK::Menu);
}

bool CompositionHostWindow::isCommandPressed() const
{
    return isKeyPressed(VK::LWin) || isKeyPressed(VK::RWin);
}

ModifierKeys CompositionHostWindow::getModifiers() const
{
    return {
        isShiftPressed(), isControlPressed(), isAltPressed(), isCommandPressed()};
}

void CompositionHostWindow::setMouseLocked(bool locked)
{
    if (mouseLockIntent == locked)
        return;

    mouseLockIntent = locked;

    if (locked && GetFocus() == hwnd)
        engageMouseLock();
    else if (!locked)
        disengageMouseLock();
}

void CompositionHostWindow::engageMouseLock()
{
    if (mouseLockEngaged)
        return;

    mouseLockEngaged = true;
    ShowCursor(FALSE);
    clipCursorToClient();

    auto center = clientCenter();
    ClientToScreen(hwnd, &center);
    SetCursorPos(center.x, center.y);
}

void CompositionHostWindow::disengageMouseLock()
{
    if (!mouseLockEngaged)
        return;

    mouseLockEngaged = false;
    ClipCursor(nullptr);
    ShowCursor(TRUE);
}

void CompositionHostWindow::clipCursorToClient() const
{
    RECT client {};
    GetClientRect(hwnd, &client);

    POINT topLeft {client.left, client.top};
    POINT bottomRight {client.right, client.bottom};
    ClientToScreen(hwnd, &topLeft);
    ClientToScreen(hwnd, &bottomRight);

    RECT screenRect {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    ClipCursor(&screenRect);
}

POINT CompositionHostWindow::clientCenter() const
{
    RECT client {};
    GetClientRect(hwnd, &client);
    return {client.right / 2, client.bottom / 2};
}

void CompositionHostWindow::handleLockedMouseMove(LPARAM lParam)
{
    auto center = clientCenter();
    auto x = getXFromLParam(lParam);
    auto y = getYFromLParam(lParam);

    // The echo of our own recentering SetCursorPos carries no motion.
    if (x == center.x && y == center.y)
        return;

    auto screenCenter = center;
    ClientToScreen(hwnd, &screenCenter);
    SetCursorPos(screenCenter.x, screenCenter.y);

    if (!contentView)
        return;

    auto scale = getDpiScale();
    MouseEvent event;
    event.type = MouseEventType::Moved;
    event.modifiers = getModifiers();
    event.pos = {static_cast<float>(center.x) / scale,
                 static_cast<float>(center.y) / scale};
    event.delta = {static_cast<float>(x - center.x) / scale,
                   static_cast<float>(y - center.y) / scale};
    dispatchMouseToContentView(event);
}

void CompositionHostWindow::teardown()
{
    disengageMouseLock();

    if (rootVisual)
        rootVisual->RemoveAllVisuals();

    rootVisual.Reset();
    target.Reset();
    commitComposition();

    EA::Vectors::eraseIf(compositionHosts(),
                         [this](auto* host) { return host == this; });

    if (contentView)
        unregisterContentViewHwnd(contentView);

    if (hwnd)
        DestroyWindow(hwnd);
}

void CompositionHostWindow::resizeContentViewToClient()
{
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    auto scale = getDpiScale();
    auto widthInPoints = static_cast<float>(clientRect.right) / scale;
    auto heightInPoints = static_cast<float>(clientRect.bottom) / scale;

    if (contentView)
    {
        contentView->setBounds(Rect(0, 0, widthInPoints, heightInPoints));
        ensureAllLayersRendered(contentView);
    }

    if (onContentResized)
        onContentResized(static_cast<int>(widthInPoints),
                         static_cast<int>(heightInPoints));
}

void CompositionHostWindow::ensureMouseLeaveTracking()
{
    if (trackingMouseLeave)
        return;

    TRACKMOUSEEVENT tme {};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd;
    TrackMouseEvent(&tme);
    trackingMouseLeave = true;
}

void CompositionHostWindow::dispatchMouseToContentView(const MouseEvent& event) const
{
    contentView->dispatchMouseEvent(event);
    ensureAllLayersRendered(contentView);
}

std::optional<LRESULT> CompositionHostWindow::handleCommonMessage(UINT msg,
                                                                  WPARAM wParam,
                                                                  LPARAM lParam)
{
    switch (msg)
    {
        case WM_SIZE:
            resizeContentViewToClient();
            InvalidateRect(hwnd, nullptr, FALSE);

            if (mouseLockEngaged)
                clipCursorToClient();
            return 0;

        // The clip rectangle is in screen coordinates, so a moved window
        // needs it refreshed to keep confining the pinned cursor.
        case WM_MOVE:
            if (mouseLockEngaged)
                clipCursorToClient();
            return std::nullopt;

        case WM_SETFOCUS:
            if (mouseLockIntent)
                engageMouseLock();
            return std::nullopt;

        case WM_KILLFOCUS:
            disengageMouseLock();
            // The matching key-ups go to whoever took focus (e.g. Alt+Tab), so
            // tracked state would otherwise report keys stuck down forever.
            keyState.reset();
            return std::nullopt;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            paintDirtyViewsForHost(hwnd);
            if (contentView)
                ensureAllLayersRendered(contentView);
            return 0;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if (contentView)
            {
                // Capture the mouse so a drag keeps delivering moves even when
                // the cursor leaves the client area (matching NSView tracking).
                SetCapture(hwnd);

                auto event = makeMouseEvent(
                    lParam, getDpiScale(), MouseEventType::Down, getModifiers());
                event.button = buttonForDown(msg);
                mouseButtonHeld = true;
                heldMouseButton = event.button;
                dispatchMouseToContentView(event);
            }
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            // Clear before ReleaseCapture: it sends WM_CAPTURECHANGED, which
            // must not synthesize a second Up for this gesture.
            mouseButtonHeld = false;

            if (contentView)
            {
                auto event = makeMouseEvent(
                    lParam, getDpiScale(), MouseEventType::Up, getModifiers());
                event.button = buttonForUp(msg);
                dispatchMouseToContentView(event);
            }

            // Release the capture once no buttons remain held.
            if ((wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) == 0)
                ReleaseCapture();
            return 0;

        case WM_CAPTURECHANGED:
            // Capture stolen mid-drag (OS move loop, drag-drop, a popup): the
            // matching WM_*BUTTONUP will never arrive here, so synthesize the
            // Up to unwind the captured view's drag state.
            synthesizeMouseUpOnCaptureLoss();
            return 0;

        case WM_MOUSEMOVE:
        {
            if (mouseLockEngaged)
            {
                handleLockedMouseMove(lParam);
                return 0;
            }

            // Arm a WM_MOUSELEAVE so a cursor leaving the surface produces an
            // Exited event, clearing any hover (e.g. a WebView's :hover state).
            ensureMouseLeaveTracking();

            if (contentView)
            {
                auto buttons = wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON);

                // A move with a button held is a drag. dispatchMouseEvent only
                // forwards Dragged/Up to the captured mouseDownTarget; a plain
                // Moved is re-hit-tested, so without this the title-bar grab is
                // lost the instant the cursor moves and panels never drag.
                auto event = makeMouseEvent(lParam,
                                            getDpiScale(),
                                            buttons != 0 ? MouseEventType::Dragged
                                                         : MouseEventType::Moved,
                                            getModifiers());
                if (buttons != 0)
                    event.button = (wParam & MK_LBUTTON)   ? MouseButton::Left
                                   : (wParam & MK_RBUTTON) ? MouseButton::Right
                                                           : MouseButton::Middle;
                dispatchMouseToContentView(event);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingMouseLeave = false;
            if (contentView)
            {
                MouseEvent event;
                event.type = MouseEventType::Exited;
                event.modifiers = getModifiers();
                dispatchMouseToContentView(event);
            }
            return 0;

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (contentView)
            {
                // Wheel messages carry screen coordinates (unlike the button and
                // move messages), so map them into the client area before
                // hit-testing the view under the cursor.
                POINT pt = {getXFromLParam(lParam), getYFromLParam(lParam)};
                ScreenToClient(hwnd, &pt);

                auto scale = getDpiScale();
                MouseEvent event;
                event.pos = {static_cast<float>(pt.x) / scale,
                             static_cast<float>(pt.y) / scale};
                event.type = MouseEventType::Wheel;
                event.modifiers = getModifiers();

                auto wheelDelta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam));
                event.delta = (msg == WM_MOUSEWHEEL) ? Point {0.f, wheelDelta}
                                                     : Point {wheelDelta, 0.f};
                dispatchMouseToContentView(event);
            }
            return 0;

        case WM_KEYDOWN:
        case WM_KEYUP:
            dispatchKeyEvent(msg, wParam, lParam);
            return 0;

        // Alt and Alt+key arrive as sys-key messages. Track and dispatch them
        // like plain keys, but fall through to DefWindowProc so the system
        // chords (Alt+F4, Alt+Space, ...) keep working.
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            dispatchKeyEvent(msg, wParam, lParam);
            return std::nullopt;
    }

    return std::nullopt;
}

void CompositionHostWindow::dispatchKeyEvent(UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    auto vk = static_cast<uint16_t>(wParam);

    if (vk < 256)
        keyState.set(vk, down);

    if (!contentView)
        return;

    KeyEvent event;
    event.keyCode = keyCodeFromVirtualKey(vk);
    event.type = down ? KeyEventType::Down : KeyEventType::Up;
    event.modifiers = getModifiers();

    if (down)
    {
        event.characters = takePendingCharacters();
        event.isRepeat = (lParam & 0x40000000) != 0;
        contentView->keyDown(event);
    }
    else
    {
        contentView->keyUp(event);
    }

    ensureAllLayersRendered(contentView);
}

// The pump's TranslateMessage has already posted any WM_CHAR / WM_SYSCHAR for
// the key being dispatched; pull them off the queue so the KeyEvent carries the
// layout-aware text — the equivalent of NSEvent.characters on macOS, and what
// makes text entry work on non-US layouts (including dead keys).
std::string CompositionHostWindow::takePendingCharacters() const
{
    auto wide = std::wstring {};
    auto charMsg = MSG {};

    while (PeekMessageW(&charMsg, hwnd, WM_CHAR, WM_CHAR, PM_REMOVE)
           || PeekMessageW(&charMsg, hwnd, WM_SYSCHAR, WM_SYSCHAR, PM_REMOVE))
        wide.push_back(static_cast<wchar_t>(charMsg.wParam));

    return fromWideString(wide);
}

void CompositionHostWindow::synthesizeMouseUpOnCaptureLoss()
{
    if (!mouseButtonHeld)
        return;

    mouseButtonHeld = false;

    if (!contentView)
        return;

    auto pt = POINT {};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);

    auto scale = getDpiScale();
    MouseEvent event;
    event.pos = {static_cast<float>(pt.x) / scale, static_cast<float>(pt.y) / scale};
    event.type = MouseEventType::Up;
    event.button = heldMouseButton;
    event.modifiers = getModifiers();
    dispatchMouseToContentView(event);
}

} // namespace eacp::Graphics
