#include "App.h"

#include <cassert>

namespace eacp::Apps
{
// No Dock/activation-policy concept here.
void setDockIconVisible(bool) {}

// Linux has no binary-signing convention to check against.
bool isDistributionSigned()
{
    return false;
}

// TODO: wire to xdg-open via fork/exec (avoid system() — shell metachars
// in URLs are a hazard).
void openExternalURL(const std::string&)
{
    assert(false && "openExternalURL not implemented on Linux");
}

// TODO: wire to a portal (xdg-desktop-portal FileChooser) or GTK dialog.
std::optional<std::string> chooseFile(const FilePickerOptions&)
{
    return std::nullopt;
}

// TODO: wire to a portal (xdg-desktop-portal FileChooser) or GTK dialog.
std::optional<std::string> chooseDirectory()
{
    return std::nullopt;
}
} // namespace eacp::Apps
