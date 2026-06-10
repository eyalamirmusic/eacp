#include <eacp/Core/Utils/WinInclude.h>

#include "CompositionHostWindow-Windows.h"
#include "../Helpers/StringUtils-Windows.h"
#include "../Layers/NativeLayer-Windows.h"

#include <unordered_map>

#include <Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>

namespace eacp::Graphics
{

// paintDirtyViewsForHost() is defined in View-Windows.cpp; getWinRTCompositor()
// is declared by NativeLayer-Windows.h. Both are linked earlier in the unity
// build.
void paintDirtyViewsForHost(HWND host);

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
} // namespace

// Called by the rendering-device recovery in D2DFactory-Windows.cpp: the
// replaced device keeps all composition surfaces alive but discards their
// pixels, so every layer re-renders and every painting view repaints.
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

    auto compositor = getWinRTCompositor();
    if (!compositor)
        return;

    auto interop =
        compositor
            .as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    winrt::com_ptr<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget>
        abiTarget;
    auto hr =
        interop->CreateDesktopWindowTarget(hwnd, topMost ? 1 : 0, abiTarget.put());
    if (FAILED(hr) || !abiTarget)
        return;

    target = abiTarget.as<wuc::Desktop::DesktopWindowTarget>();
    rootVisual = compositor.CreateContainerVisual();
    rescaleRootVisualToDpi();
    target.Root(rootVisual);
}

float CompositionHostWindow::getDpiScale() const
{
    auto dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return static_cast<float>(dpi) / 96.f;
}

void CompositionHostWindow::rescaleRootVisualToDpi()
{
    if (!rootVisual)
        return;

    auto scale = getDpiScale();
    rootVisual.Scale({scale, scale, 1.0f});
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

    auto* viewVisual = static_cast<wuc::ContainerVisual*>(view->getHandle());

    if (rootVisual && viewVisual)
        rootVisual.Children().InsertAtTop(*viewVisual);

    ensureAllLayersRendered(view);
}

void CompositionHostWindow::ensureAllLayersRendered(const View* view) const
{
    ensureAllLayersRendered(view, getDpiScale());
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

void CompositionHostWindow::teardown()
{
    if (rootVisual)
        rootVisual.Children().RemoveAll();

    rootVisual = nullptr;
    target = nullptr;

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
            return 0;

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

        case WM_KILLFOCUS:
            // The matching key-ups go to whoever took focus (e.g. Alt+Tab), so
            // tracked state would otherwise report keys stuck down forever.
            keyState.reset();
            return 0;
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
