#pragma once

#include <functional>
#include <string>

namespace eacp::Graphics
{
class TrayIcon;
}

namespace term::Notifier
{
// Sets up the platform notification center and the click handler: activating
// a delivered notification calls onActivate (on the main thread) with the
// session key it was posted for. Call once at startup.
void initialize(std::function<void(const std::string& sessionKey)> onActivate);

// Windows shows notifications as balloons anchored to the app's tray icon,
// so the tray hands itself over here; macOS uses the notification center
// and ignores this. The icon must outlive all notify() calls.
void attachTray(eacp::Graphics::TrayIcon& icon);

// Posts a desktop notification attributed to a session. Clicking it brings
// the app forward and jumps to that session.
void notify(const std::string& sessionKey,
            const std::string& title,
            const std::string& body);
} // namespace term::Notifier
