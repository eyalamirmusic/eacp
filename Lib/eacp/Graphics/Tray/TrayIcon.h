#pragma once

#include "../Image/Image.h"
#include "../Menu/Menu.h"

namespace eacp::Graphics
{

// A status item living in the macOS menu bar / Windows notification area
// ("system tray"). Construct one and keep it alive for as long as the icon
// should be visible; destroying it removes the icon.
//
// A TrayIcon needs no Window of its own. Combine it with
// Apps::setDockIconVisible(false) to build a menu-bar / tray-only app that
// has no Dock icon (macOS) and no taskbar button (Windows). See the TrayApp
// example.
//
// All callbacks fire on the main thread. Under headless mode every method is
// a no-op, mirroring Window.
class TrayIcon
{
public:
    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // The icon shown in the menu bar / tray. Supply a square RGBA image
    // (32x32 or larger for crisp Retina). On macOS it's drawn as a template
    // by default — tinted to match the menu bar via its alpha channel —
    // unless setTemplateRendering(false) is called first.
    void setIcon(const Image& icon);

    // The hover tooltip.
    void setTooltip(const std::string& text);

    // The menu shown when the icon is clicked (macOS) or right-clicked
    // (Windows). Pass an eacp::Graphics::Menu; item actions fire on the
    // main thread. Replacing the menu replaces it wholesale.
    void setMenu(const Menu& menu);

    // Invoked on a plain left-click. On macOS this only fires when no menu
    // is set, because a menu takes over the click. On Windows it fires on a
    // left-click regardless of whether a (right-click) menu is set.
    void setOnClick(Callback callback);

    // macOS only: whether the icon is drawn as a template image (system
    // tinted, alpha-only). No-op on other platforms. Defaults to true.
    void setTemplateRendering(bool shouldRenderAsTemplate);

    // Windows only: posts a desktop notification anchored to this tray icon
    // (a balloon, shown as a toast on Windows 10+). No-op elsewhere — macOS
    // apps use the system notification center instead.
    void showNotification(const std::string& title, const std::string& body);

    // Windows only: invoked on the main thread when a notification posted
    // via showNotification is clicked.
    void setOnNotificationClick(Callback callback);

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
