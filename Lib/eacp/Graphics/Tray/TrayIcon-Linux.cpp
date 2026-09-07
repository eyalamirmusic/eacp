#include "TrayIcon.h"

// A status icon needs StatusNotifierItem over D-Bus, which is not built here,
// so every operation is a no-op as it is on iOS.

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
