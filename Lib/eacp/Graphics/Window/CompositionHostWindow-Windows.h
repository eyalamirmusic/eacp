#pragma once

#include "../DComp-Windows.h"

#include "../View/View.h"

#include <bitset>

namespace eacp::Graphics
{

// Maps a content view's root to the HWND hosting it. Main thread only.
void registerContentViewHwnd(View* root, HWND hwnd);
void unregisterContentViewHwnd(View* root);
HWND findHostHwndForView(View* view);

// Content compositing straight to the screen (a GPUView's swapchain) must
// respect its own alpha when true. False for an unknown/null HWND.
bool isHostWindowTransparent(HWND hwnd);

void repaintViewTree(View* view);

// Shared by the two Windows surfaces, Window and EmbeddedView; each registers
// its own window class and creates its HWND, then drives this for the rest.
struct CompositionHostWindow
{
    // topMost mirrors the CreateDesktopWindowTarget flag (Window = true,
    // EmbeddedView = false).
    void initializeComposition(bool topMost);

    float getDpiScale() const;
    void rescaleRootVisualToDpi();

    void attachContentView(View* view);
    void ensureAllLayersRendered(const View* view) const;

    bool isKeyPressed(uint16_t vk) const;
    bool isShiftPressed() const;
    bool isControlPressed() const;
    bool isAltPressed() const;
    bool isCommandPressed() const;
    ModifierKeys getModifiers() const;

    // Hides, clips and recenters the cursor, streaming motion as Moved events
    // carrying MouseEvent::delta in points. Suspended while the HWND lacks
    // focus.
    void setMouseLocked(bool locked);
    bool isMouseLocked() const { return mouseLockIntent; }

    // Call from the owning surface's destructor.
    void teardown();

    // nullopt for messages it does not own, so the surface's WndProc can fall
    // back to its own handling and DefWindowProcW.
    std::optional<LRESULT>
        handleCommonMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd = nullptr;
    View* contentView = nullptr;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> rootVisual;

    // The composition generation this target/root was built under; falling
    // behind means a device loss killed both. See DComp-Windows.h.
    uint64_t generation = 0;
    bool topMostTarget = false;

    // See WindowOptions::transparentBackground: no redirection surface, so
    // there is no bitmap behind the visual tree to fill.
    bool transparentBackground = false;

    std::bitset<256> keyState;
    bool trackingMouseLeave = false;

    // Button-down seen, matching up not yet dispatched.
    bool mouseButtonHeld = false;
    MouseButton heldMouseButton = MouseButton::Left;

    // Fired after a WM_SIZE updates the content-view bounds; sizes in points.
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

    // Raw Input is the only source of unaccelerated device movement; see
    // MouseEvent::rawDelta.
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

    // Nothing else distinguishes a restore's WM_SIZE from an ordinary resize.
    bool hostMinimized = false;
};

} // namespace eacp::Graphics
