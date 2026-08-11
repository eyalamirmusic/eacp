#pragma once

#include "../Image/Image.h"
#include "../Menu/Menu.h"

namespace eacp::Graphics
{

// A status item in the macOS menu bar / Windows notification area; destroying
// it removes the icon. Needs no Window. All callbacks fire on the main thread,
// and every method is a no-op under headless.
class TrayIcon
{
public:
    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Square RGBA, 32x32 or larger for Retina. Drawn as a template on macOS
    // unless setTemplateRendering(false) was called first.
    void setIcon(const Image& icon);

    void setTooltip(const std::string& text);

    // Shown on click (macOS) or right-click (Windows). Replaces wholesale.
    void setMenu(const Menu& menu);

    // Left-click. On macOS only fires when no menu is set, since a menu takes
    // over the click.
    void setOnClick(Callback callback);

    // macOS only, default true: draws the icon system-tinted and alpha-only.
    void setTemplateRendering(bool shouldRenderAsTemplate);

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
