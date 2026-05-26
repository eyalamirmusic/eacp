#pragma once

#include <string>
#include <ea_data_structures/Structures/Vector.h>
#include "../View/View.h"
#include <eacp/Core/App/App.h>

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

struct WindowOptions
{
    WindowOptions()
    {
        flags.emplace_back(WindowFlags::Titled);
        flags.emplace_back(WindowFlags::Closable);
        flags.emplace_back(WindowFlags::Miniaturizable);
        flags.emplace_back(WindowFlags::Resizable);
    }

    // When the user closes the window. If left empty, falls back to
    // Apps::quit when isPrimary is true, or a no-op otherwise.
    Callback onQuit {};

    // Set to false for secondary/popup windows so closing them doesn't
    // terminate the app when onQuit is unset.
    bool isPrimary = true;

    // Called after the window has been resized. Sizes are in points and refer
    // to the content view, not the outer frame.
    ResizeCallback onResize {};

    // Called while the user is dragging the resize corner. Receives the
    // proposed content-view size in points; may be mutated to clamp or
    // snap to constraints.
    WillResizeCallback onWillResize {};

    int width = 640;
    int height = 400;
    std::string title = "New Window";

    // When false, the title bar still shows but the title text is hidden.
    bool showTitle = true;

    // macOS only: makes the title bar background transparent so the
    // content view's color shows through. Pair with FullSizeContentView
    // for an edge-to-edge look. No-op on Windows.
    bool transparentTitlebar = false;

    EA::Vector<WindowFlags> flags;

    Callback effectiveOnQuit() const
    {
        if (onQuit)
            return onQuit;
        return isPrimary ? Callback {Apps::quit} : Callback {[] {}};
    }
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

    void setContentView(View& view);

    // Keyboard state (tracked from window events)
    bool isKeyPressed(uint16_t virtualKeyCode) const;
    bool isShiftPressed() const;
    bool isControlPressed() const;
    bool isAltPressed() const;
    bool isCommandPressed() const;
    ModifierKeys getModifiers() const;

private:
    WindowOptions options;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics