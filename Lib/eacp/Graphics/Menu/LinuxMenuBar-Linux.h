#pragma once

#include "Menu.h"
#include "../Window/WindowInput.h"

#include <memory>

// The menu bar of a Linux window. No window system there has a menu protocol,
// so the bar is drawn in the window, over its top, with the content laid out
// below it. eacp-graphics has no text on Linux, so it lays the bar out, routes
// input to it and binds its shortcuts, and eacp-ui draws it: it registers a
// presenter factory here (UI/Widgets/MenuBarPresenter-Linux.cpp). An app that
// links no eacp-ui gets the shortcuts and no bar.

namespace eacp::Graphics
{
class LinuxWindowNative;
struct LinuxWindowSurface;

// What draws one window's bar. Its view is a root of its own, bound to the
// window beside the content view.
class LinuxMenuBarPresenter
{
public:
    virtual ~LinuxMenuBarPresenter() = default;

    virtual View& getView() = 0;

    // Points the bar takes off the top of the window.
    virtual float getHeight() const = 0;

    // Places the view in a window this big: the bar along the top, or while
    // a menu is open as much of the window as the menu draws in.
    virtual void layout(Rect windowBounds) = 0;

    virtual bool isMenuOpen() const = 0;
    virtual void dismissMenu() = 0;

    // A key the window got. While a menu is open the menu takes every key
    // and this is true; otherwise false, and the key goes on.
    virtual bool keyPressed(const KeyEvent& event) = 0;
};

// `layoutChanged` is the presenter's to call when a menu opens or closes, so
// the window lays the view out again and routes the pointer to match.
using LinuxMenuBarPresenterFactory =
    std::function<std::unique_ptr<LinuxMenuBarPresenter>(const MenuBar& bar,
                                                         Callback layoutChanged)>;

void setLinuxMenuBarPresenterFactory(LinuxMenuBarPresenterFactory factory);

// One window's installed bar: the shortcuts, and the presenter where there is
// one. Owned by the window's LinuxWindowState.
class LinuxWindowMenuBar final : WindowInputListener
{
public:
    LinuxWindowMenuBar(LinuxWindowNative& nativeToUse, const MenuBar& barToUse);
    ~LinuxWindowMenuBar() override;

    LinuxWindowMenuBar(const LinuxWindowMenuBar&) = delete;
    LinuxWindowMenuBar& operator=(const LinuxWindowMenuBar&) = delete;

    float getHeight() const;

    // Takes the window's routing over: the bar's view, its layout and the key
    // filter. Done once the bar it replaces is gone.
    void attach();

private:
    void layout();
    bool filterKey(const KeyEvent& event);
    void windowActivationChanged(bool isKey) override;

    LinuxWindowNative& native;
    LinuxWindowSurface& surface;
    WindowInputTap& input;
    MenuBar bar;
    std::unique_ptr<LinuxMenuBarPresenter> presenter;
};
} // namespace eacp::Graphics
