#pragma once

#include "Config.h"
#include "Palette.h"
#include "Session.h"

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

    std::function<void(const std::string&)> onWindowTitleChanged =
        [](const std::string&) {};
    eacp::Callback onBringToFront = [] {};

    void resized() override;

private:
    bool interceptKey(const eacp::Graphics::KeyEvent& event);
    bool handlePrefixed(const eacp::Graphics::KeyEvent& event);
    bool handleCommand(const eacp::Graphics::KeyEvent& event);
    void showPalette();
    void hidePalette();
    void attachActive(TermSession& session);
    void wireViews();
    void updateTitle();
    void handleSessionNotify(TermSession& session, const std::string& text);

    AppConfig config = loadConfig();
    SessionManager manager {config};
    Palette palette {config, manager};
    TermSession* attached = nullptr;
    bool prefixArmed = false;
};
} // namespace term
