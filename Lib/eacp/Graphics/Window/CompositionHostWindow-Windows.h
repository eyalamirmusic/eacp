#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include "../View/View.h"

#include <bitset>
#include <cstdint>
#include <functional>
#include <optional>

#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

// Maps a content view's root to the HWND hosting it. Both the top-level Window
// and the child EmbeddedView register through CompositionHostWindow, so a
// WebView (or any repaint-driven View) nested in either surface can resolve its
// host HWND. Main-thread only, so no locking is needed.
void registerContentViewHwnd(View* root, HWND hwnd);
void unregisterContentViewHwnd(View* root);
HWND findHostHwndForView(View* view);

// Marks `view` and all its subviews for repaint, e.g. after a DPI change or a
// rendering-device replacement invalidates every backing surface.
void repaintViewTree(View* view);

// The composition-hosted HWND machinery shared by the two Windows surfaces: the
// top-level Window and the child EmbeddedView. It owns the DesktopWindowTarget,
// the root visual, the content view, DPI, keyboard state, and the WndProc
// message handling common to both. Each surface still registers its own window
// class (carrying its own static WndProc) and creates its HWND, then drives this
// for everything shared.
struct CompositionHostWindow
{
    // topMost mirrors the CreateDesktopWindowTarget flag (Window = true,
    // EmbeddedView = false).
    void initializeComposition(bool topMost);

    float getDpiScale() const;
    void rescaleRootVisualToDpi();

    // Binds the content view: sizes it to the client area, inserts its visual,
    // registers the host HWND, and renders its layers.
    void attachContentView(View* view);
    void ensureAllLayersRendered(const View* view) const;

    bool isKeyPressed(uint16_t vk) const;
    bool isShiftPressed() const;
    bool isControlPressed() const;
    bool isAltPressed() const;
    bool isCommandPressed() const;
    ModifierKeys getModifiers() const;

    // Mouse lock (relative-motion mode): hides the cursor, clips it to the
    // client area and recenters it after every move, streaming the motion as
    // Moved events whose MouseEvent::delta carries the movement in points.
    // The lock expresses intent: it engages while the HWND has focus and
    // WM_SETFOCUS / WM_KILLFOCUS re-engage / suspend it.
    void setMouseLocked(bool locked);
    bool isMouseLocked() const { return mouseLockIntent; }

    // Tears down the visual tree, registry entry, and HWND. Call from the
    // owning surface's destructor.
    void teardown();

    // Handles the messages shared by both surfaces (paint, size, mouse buttons,
    // move/drag with capture, wheel, mouse-leave, keyboard). Returns nullopt for
    // messages it doesn't own, so the surface's WndProc can fall back to its own
    // handling and DefWindowProcW.
    std::optional<LRESULT>
        handleCommonMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd = nullptr;
    View* contentView = nullptr;
    wuc::Desktop::DesktopWindowTarget target {nullptr};
    wuc::ContainerVisual rootVisual {nullptr};
    std::bitset<256> keyState;
    bool trackingMouseLeave = false;

    // Button-down seen, matching up not yet dispatched. Lets WM_CAPTURECHANGED
    // synthesize the Up a stolen capture would otherwise swallow.
    bool mouseButtonHeld = false;
    MouseButton heldMouseButton = MouseButton::Left;

    // Fired after a WM_SIZE updates the content-view bounds, with the new
    // content size in points. The top-level Window wires this to
    // WindowOptions::onResize; EmbeddedView leaves it unset.
    std::function<void(int widthInPoints, int heightInPoints)> onContentResized;

private:
    void engageMouseLock();
    void disengageMouseLock();
    void clipCursorToClient() const;
    POINT clientCenter() const;
    void handleLockedMouseMove(LPARAM lParam);

    void resizeContentViewToClient();
    void ensureMouseLeaveTracking();
    void dispatchMouseToContentView(const MouseEvent& event) const;
    void ensureAllLayersRendered(const View* view, float dpiScale) const;
    void dispatchKeyEvent(UINT msg, WPARAM wParam, LPARAM lParam);
    void synthesizeMouseUpOnCaptureLoss();
    std::string takePendingCharacters() const;

    bool mouseLockIntent = false;
    bool mouseLockEngaged = false;
};

} // namespace eacp::Graphics
