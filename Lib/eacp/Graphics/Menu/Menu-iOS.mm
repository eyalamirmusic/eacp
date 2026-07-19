#include "Menu.h"

namespace eacp::Graphics
{
// iOS has no menu bar of either kind, so the model is accepted and discarded —
// portable code can build one unconditionally.
void setApplicationMenuBar(const MenuBar&, Window&)
{
}

Menu standardApplicationMenu(std::string applicationName)
{
    return Menu {std::move(applicationName)};
}

Menu standardEditMenu()
{
    return Menu {"Edit"};
}

} // namespace eacp::Graphics
