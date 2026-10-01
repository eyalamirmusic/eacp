#pragma once

#include "Session.h"

#include <eacp/Graphics/Tray/TrayIcon.h>

namespace term
{
// The menu-bar presence that makes hide-on-close feel intentional: close
// the window and the icon stays, shells keep running. The menu lists every
// session (Claude sessions marked) for a one-click jump back.
class TrayController
{
public:
    explicit TrayController(SessionManager& sessionsToUse);

    // Rebuilds the menu; call when sessions, the active session or notify
    // state change.
    void refresh();

    eacp::Callback onShowWindow = [] {};
    std::function<void(TermSession&)> onPickSession = [](TermSession&) {};

private:
    static eacp::Graphics::Image makeIcon();

    SessionManager& sessions;
    eacp::Graphics::TrayIcon icon;
};
} // namespace term
