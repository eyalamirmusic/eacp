#pragma once

#include "../DComp-Windows.h"

#include "../View/View.h"

#include <bitset>

namespace eacp::Graphics
{

// Maps a content view's root to the HWND hosting it. Both the top-level Window
// and the child EmbeddedView register through CompositionHostWindow, so a
// WebView (or any repaint-driven View) nested in either surface can resolve its
// host HWND. Main-thread only, so no locking is needed.
void registerContentViewHwnd(View* root, HWND hwnd);
void unregisterContentViewHwnd(View* root);
HWND findHostHwndForView(View* view);

// Whether `hwnd` hosts a WindowOptions::transparentBackground surface. Content
// that composites straight to the screen (a GPUView's swapchain) must respect
// its own alpha there — an opaque swapchain would paint the black box the
// option exists to remove. False for an unknown/null HWND.
bool isHostWindowTransparent(HWND hwnd);

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

    // Overrides what getDpiScale reports, for a surface whose host does its own
    // scaling and so works in a figure the window's DPI does not agree with; 0
    // goes back to that DPI. Everything measured through getDpiScale follows —
    // the root visual's transform, the size in points the content view is
    // given, where a mouse event lands — so the surface stays consistent with
    // itself whichever answer is in force.
    void setDpiScaleOverride(float scale);

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
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> rootVisual;

    // The composition generation this target/root was built under. DComp cannot
    // swap its rendering device, so a device loss makes both dead objects — see
    // DComp-Windows.h. rebuildAllCompositionHosts() re-runs initializeComposition
    // when this falls behind.
    uint64_t generation = 0;
    bool topMostTarget = false;

    // See WindowOptions::transparentBackground: the HWND was created with no
    // redirection surface, so there is no bitmap behind the visual tree to
    // fill — and filling it is what would make the window opaque again.
    bool transparentBackground = false;

    // See setDpiScaleOverride. 0 means the window's own DPI answers.
    float dpiScaleOverride = 0.f;

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

    void fillWindowBackground(HDC dc) const;
    void resizeContentViewToClient();
    void ensureMouseLeaveTracking();
    void dispatchMouseToContentView(MouseEvent event);

    POINT toScreenPixels(Point position) const;
    Point fromScreenPixels(POINT screenPixels) const;

    // Where the held button went down, in screen pixels — see
    // dispatchMouseToContentView for why it is not kept in client points.
    POINT mouseDownScreenPosition {};

    // The mouse's own movement, which the ordinary pointer messages cannot
    // report: they carry the pointer's position after the system's acceleration
    // curve has shaped it, and after it has been rounded to whole pixels and
    // stopped at the edges of the screen. A camera needs the device's figures —
    // see MouseEvent::rawDelta — and Raw Input is where Windows keeps them.
    void ensureRawMouseRegistered();
    void accumulateRawMouseMovement(LPARAM lParam);

    bool rawMouseRegistered = false;
    Point rawMouseMovement;
    void ensureAllLayersRendered(const View* view, float dpiScale) const;
    void dispatchKeyEvent(UINT msg, WPARAM wParam, LPARAM lParam);
    void synthesizeMouseUpOnCaptureLoss();
    std::string takePendingCharacters() const;

    bool mouseLockIntent = false;
    bool mouseLockEngaged = false;

    // A minimized window is reported to the view tree as hidden, so the restore
    // that follows has to report it visible again. Nothing else distinguishes
    // that WM_SIZE from an ordinary resize.
    bool hostMinimized = false;
};

} // namespace eacp::Graphics
