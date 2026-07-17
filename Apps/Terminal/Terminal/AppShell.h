#pragma once

#include "Config.h"
#include "Palette.h"
#include "Popup.h"
#include "Session.h"
#include "TrayController.h"

namespace term
{
// Root content view: hosts the active session's terminal, the palette
// overlay, the Ctrl+A leader-key table and notification routing.
class AppShell final : public eacp::Graphics::View
{
public:
    AppShell();

    // Call once the window is up: restores the saved workspace (or creates
    // the first session) and focuses it.
    void start();

    // Wire to WindowEvents::onActivationChanged: notifications for the
    // active session are suppressed only while the user is actually looking
    // at it, which needs the window's key state.
    void setWindowFocused(bool focused) { windowFocused = focused; }

    std::function<void(const std::string&)> onWindowTitleChanged =
        [](const std::string&) {};
    eacp::Callback onBringToFront = [] {};

    void resized() override;

private:
    bool interceptKey(const eacp::Graphics::KeyEvent& event);
    bool handlePrefixed(const eacp::Graphics::KeyEvent& event);
    bool handleCommand(const eacp::Graphics::KeyEvent& event);
    bool popupKey(const eacp::Graphics::KeyEvent& event);
    void showPalette();
    void hidePalette();
    void showPopup(const std::string& command);
    void hidePopup();
    void attachActive(TermSession& session);
    void updateTitle();
    void handleSessionNotify(TermSession& session, const std::string& text);

    AppConfig config = loadConfig();
    SessionManager manager {config};
    Palette palette {config, manager};
    Popup popup {config};
    TrayController tray {manager};
    TermSession* attached = nullptr;
    bool prefixArmed = false;
    bool popupPrefixArmed = false;
    bool windowFocused = true;
};
} // namespace term
