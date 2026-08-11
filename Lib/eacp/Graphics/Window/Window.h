#pragma once

#include "../Image/Image.h"
#include "../Primitives/Primitives.h"
#include "../View/View.h"

namespace eacp::Graphics
{

enum class WindowFlags
{
    Borderless,
    Titled,
    Closable,
    Miniaturizable,
    Resizable,
    UnifiedTitleAndToolbar,
    FullScreen,
    FullSizeContentView,
    UtilityWindow,
    DocModalWindow,
    NonactivatingPanel,
    HUDWindow
};

using ResizeCallback = std::function<void(int width, int height)>;
using WillResizeCallback = std::function<void(int& width, int& height)>;

// All handlers fire on the main thread.
struct WindowEvents
{
    std::function<void(bool isKey)> onActivationChanged;
};

struct WindowOptions
{
    WindowOptions()
    {
        flags.add({WindowFlags::Titled,
                   WindowFlags::Closable,
                   WindowFlags::Miniaturizable,
                   WindowFlags::Resizable});
    }

    Callback effectiveOnQuit() const
    {
        if (onQuit)
            return onQuit;
        return isPrimary ? Callback {[] { Apps::quit(); }} : Callback {[] {}};
    }

    // Empty falls back to Apps::quit when isPrimary, or a no-op otherwise.
    Callback onQuit {};

    bool isPrimary = true;

    // Closing hides the window instead of destroying it; onQuit never fires and
    // the window's state stays alive until setVisible(true) or app quit.
    bool hidesOnClose = false;

    // Content-view size in points, not the outer frame.
    ResizeCallback onResize {};

    // Proposed content-view size in points while dragging the resize corner;
    // may be mutated to clamp or snap.
    WillResizeCallback onWillResize {};

    int width = 640;
    int height = 400;
    std::string title = "New Window";

    // False keeps the title bar but hides the title text.
    bool showTitle = true;

    // Drops the translucent grey band macOS paints over a FullSizeContentView.
    bool titlebarTransparent = false;

    bool showTitlebarSeparator = true;

    // macOS: inset of the window controls from the top-left, in points. Only
    // meaningful with a hidden/transparent title bar (FullSizeContentView).
    std::optional<Point> trafficLightPosition;

    // macOS: shown behind the content view before it first paints and during
    // live resize. Unset uses the system window background.
    std::optional<Color> backgroundColor;

    // Nothing is painted behind the content view, so the content defines the
    // whole window. Wins over backgroundColor. Windows uses
    // WS_EX_NOREDIRECTIONBITMAP and no frame, so cornerRadius is ignored too.
    bool transparentBackground = false;

    // Minimum content size in points; 0 = no minimum.
    int minWidth = 0;
    int minHeight = 0;

    // Locks resizing to this width-to-height ratio; only the ratio is read.
    // Give the window an initial width/height already in it — neither platform
    // retro-fits a size that was already asked for.
    std::optional<Point> aspectRatio;

    // macOS NSFloatingWindowLevel, Windows WS_EX_TOPMOST.
    bool alwaysOnTop = false;

    // macOS: pins the window to every Space. No-op on other platforms.
    bool visibleOnAllWorkspaces = false;

    // Shows the window without making it key; it can still become key on click.
    bool showInactive = false;

    // Clicks pass through to whatever is underneath. No-op on iOS.
    bool ignoresMouseEvents = false;

    // Top-left corner in screen points from the primary display's top-left.
    // Unset centers the window (macOS) / uses the system default (Windows).
    std::optional<Point> initialPosition;

    // Overrides the running app's icon (Windows: title bar, taskbar, Alt-Tab;
    // macOS: Dock tile), called once at window construction. An invalid Image
    // keeps the static icon eacp_set_app_icon bakes into the bundle/executable.
    std::function<Image()> applicationIcon = [] { return Image {}; };

    // Windows Alt-Tab big-icon slot only; an invalid Image falls back to
    // applicationIcon then the executable's embedded icon. No-op on macOS.
    std::function<Image()> altTabIcon = [] { return Image {}; };

    // Corner radius in points, for borderless windows which are square by
    // default. macOS makes the window non-opaque (overriding backgroundColor);
    // Windows 11 rounds via DWM at the system radius and ignores the value.
    std::optional<float> cornerRadius;

    EA::Vector<WindowFlags> flags;
};

struct ModifierKeys;

class Window
{
public:
    Window(const WindowOptions& optionsToUse = {});
    ~Window();

    void setTitle(const std::string& title);
    void* getHandle();
    void* getContentViewHandle();

    // Also makes the window findable via View::getWindow() from this view and
    // everything under it.
    void setContentView(View& view);

    // Also activates the app. No-op under headless and on iOS.
    void toFront();

    // Hides/shows without destroying: content view and WebView state stay alive
    // and the frame is kept. Showing respects showInactive and re-asserts
    // alwaysOnTop. No-op under headless and on iOS.
    void setVisible(bool visible);

    // False while hidden, minimized, or headless; always true on iOS.
    bool isVisible();

    // No-op under headless and on iOS.
    void minimize();

    // Restores the previous frame when already maximized. No-op under headless
    // and on iOS.
    void toggleMaximize();

    // Hides and pins the cursor; motion keeps arriving as mouseMoved events
    // carrying MouseEvent::delta. Suspends while the window is not key and
    // re-engages when focus returns.
    void setMouseLocked(bool locked);
    bool isMouseLocked() const;

    bool isKeyPressed(uint16_t virtualKeyCode) const;
    bool isShiftPressed() const;
    bool isControlPressed() const;
    bool isAltPressed() const;
    bool isCommandPressed() const;
    ModifierKeys getModifiers() const;

    WindowEvents events;

private:
    WindowOptions options;

    // Owns the back-pointer View::getWindow() reads, so window destruction
    // clears it without every platform destructor having to.
    struct ContentViewLink
    {
        ~ContentViewLink() { attach(nullptr, nullptr); }

        void attach(View* view, Window* window)
        {
            if (contentView != nullptr)
                contentView->ownerWindow = nullptr;

            contentView = view;

            if (contentView != nullptr)
                contentView->ownerWindow = window;
        }

        View* contentView = nullptr;
    };

    ContentViewLink contentLink;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
