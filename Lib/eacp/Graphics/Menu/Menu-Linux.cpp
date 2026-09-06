#include "Menu.h"

namespace eacp::Graphics
{
// A menu bar belongs to the desktop environment here, not to the window system:
// Wayland has no menu protocol, and what GNOME and KDE show comes from
// com.canonical.dbusmenu over D-Bus, exported per window. That is a real thing
// to build (plan §6), and until it is, the model is accepted and discarded the
// way iOS discards it, so portable code can build a bar unconditionally.
//
// The whole portable half — ids, ordering, enablement and checked predicates —
// is still exercised by MenuTests and MenuCommandsTests on this platform, which
// is where the mistakes that matter live.
void setApplicationMenuBar(const MenuBar&, Window&) {}

Menu standardApplicationMenu(std::string applicationName)
{
    return Menu {std::move(applicationName)};
}

Menu standardEditMenu()
{
    return Menu {"Edit"};
}
} // namespace eacp::Graphics
