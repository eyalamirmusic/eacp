#include "TrayIcon.h"

// A status icon is a desktop-environment feature rather than a window-system
// one, and the protocol for it (StatusNotifierItem over D-Bus) is the same one
// a menu bar would need. Neither exists here yet, so a TrayIcon is constructed
// and every operation is a no-op, exactly as on iOS — which is the behaviour
// TrayIcon.h already documents for headless.

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
} // namespace eacp::Graphics
