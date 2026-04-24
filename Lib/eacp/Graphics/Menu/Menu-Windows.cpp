#include "Menu.h"

namespace eacp::Graphics
{
// NOLINTNEXTLINE(performance-unnecessary-value-param)
void setApplicationMenuBar([[maybe_unused]] MenuBar bar) {}

Menu standardApplicationMenu(std::string applicationName)
{
    return Menu {std::move(applicationName)};
}

} // namespace eacp::Graphics
