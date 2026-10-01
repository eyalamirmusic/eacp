#include "TrayIcon.h"

// iOS has no menu bar / notification area, so a tray icon has nothing to
// attach to. The type still exists so cross-platform code compiles; every
// operation is a no-op.

namespace eacp::Graphics
{
struct TrayIcon::Native
{
};

TrayIcon::TrayIcon() = default;
TrayIcon::~TrayIcon() = default;

void TrayIcon::setIcon(const Image&) {}
void TrayIcon::setTooltip(const std::string&) {}
void TrayIcon::setMenu(const Menu&) {}
void TrayIcon::setOnClick(Callback) {}
void TrayIcon::setTemplateRendering(bool) {}
void TrayIcon::showNotification(const std::string&, const std::string&) {}
void TrayIcon::setOnNotificationClick(Callback) {}

} // namespace eacp::Graphics
