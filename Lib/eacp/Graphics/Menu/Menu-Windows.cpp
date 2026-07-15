#include "Menu.h"

namespace eacp::Graphics
{
void setApplicationMenuBar(const MenuBar&) {}

Menu standardApplicationMenu(std::string applicationName)
{
    return Menu {std::move(applicationName)};
}

Menu standardEditMenu()
{
    return Menu {"Edit"};
}

} // namespace eacp::Graphics
