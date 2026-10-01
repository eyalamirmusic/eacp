#include "Notifier.h"

#include <eacp/Graphics/Tray/TrayIcon.h>

namespace term::Notifier
{
namespace
{
std::function<void(const std::string&)> activateHandler = [](const std::string&) {};

eacp::Graphics::TrayIcon* tray = nullptr;

// Shell balloons carry no payload, so a click maps to the session of the
// most recently posted notification — the one the balloon is showing.
std::string lastSessionKey;
} // namespace

void initialize(std::function<void(const std::string&)> onActivate)
{
    activateHandler = std::move(onActivate);
}

void attachTray(eacp::Graphics::TrayIcon& icon)
{
    tray = &icon;
    icon.setOnNotificationClick([] { activateHandler(lastSessionKey); });
}

void notify(const std::string& sessionKey,
            const std::string& title,
            const std::string& body)
{
    if (tray == nullptr)
        return;

    lastSessionKey = sessionKey;
    tray->showNotification(title, body);
}
} // namespace term::Notifier
