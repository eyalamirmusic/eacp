#include "Menu.h"

namespace eacp::Graphics
{
// Wayland has no menu protocol, so the bar is accepted and discarded as it is
// on iOS.
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
