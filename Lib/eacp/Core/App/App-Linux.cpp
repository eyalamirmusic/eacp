#include "App.h"

#include "../Process/Process.h"

namespace eacp::Apps
{
// No Dock/activation-policy concept here.
void setDockIconVisible(bool) {}

// Badges are the desktop environment's business here (Unity launcher entries,
// per-DE D-Bus protocols) rather than the window system's, and eacp has no
// backend for any of them.
void setAppBadge(const std::string&) {}

// No OS-level power-off announcement to watch for, so a quit request is
// never the system's (see App.h).
bool isSystemPoweringOff()
{
    return false;
}

void Detail::observeSystemPowerOff() {}

// Linux has no binary-signing convention to check against.
bool isDistributionSigned()
{
    return false;
}

// posix_spawnp with the URL as an argument, never system(): a shell would read
// the metacharacters a URL is allowed to contain as its own. Detached, so the
// handler outlives both this call and the Process that started it.
void openExternalURL(const std::string& url)
{
    auto options = Processes::ProcessOptions {};
    options.executable = "xdg-open";
    options.arguments.add(url);
    options.detached = true;

    auto opener = Processes::Process {std::move(options)};
}

// TODO: wire to a portal (xdg-desktop-portal FileChooser) or GTK dialog.
std::optional<std::string> chooseFile(const FilePickerOptions&)
{
    return std::nullopt;
}

// TODO: wire to a portal (xdg-desktop-portal FileChooser) or GTK dialog.
std::optional<std::string> chooseSaveFile(const FileSaveOptions&)
{
    return std::nullopt;
}

// TODO: wire to a portal (xdg-desktop-portal FileChooser) or GTK dialog.
std::optional<std::string> chooseDirectory()
{
    return std::nullopt;
}
} // namespace eacp::Apps
