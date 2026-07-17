#pragma once

#include "Config.h"
#include "TerminalView.h"

#include <memory>
#include <string>

namespace term
{
// tmux's display-popup: an ephemeral full-window terminal running a single
// command (lazygit) over the active session. The popup dismisses itself when
// the command exits — quitting lazygit with q closes it — and focus falls
// back to the pane underneath. Children of the command (lazygit's `e`
// opening nvim) share the popup's PTY, so editors open inside the popup.
class Popup final : public eacp::Graphics::View
{
public:
    explicit Popup(const AppConfig& configToUse);

    // Spawns the command in a fresh in-process PTY; a popup's shell never
    // goes to the session daemon and dies with the popup.
    void show(const std::string& command, const std::string& workingDirectory);

    // Ends the command (the toggle case) and tears the popup down on the
    // next loop tick — never inline, since the call usually originates
    // inside one of the terminal's own callbacks.
    void dismiss();

    bool isShown() const { return terminal != nullptr; }

    // Fired once the popup is gone; the app shell removes the overlay and
    // restores pane focus.
    eacp::Callback onClosed = [] {};

    // Installed on the popup's terminal; the app shell hangs the leader key
    // here (Ctrl+A i toggles the popup closed).
    std::function<bool(const eacp::Graphics::KeyEvent&)> interceptKey =
        [](const eacp::Graphics::KeyEvent&) { return false; };

    void resized() override;
    void paint(eacp::Graphics::Context& context) override;

private:
    eacp::Graphics::Rect panelBounds() const;

    const AppConfig& config;
    Theme theme;
    std::unique_ptr<TerminalView> terminal;
    bool closing = false;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};
} // namespace term
