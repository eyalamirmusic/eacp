#include "SystemAppearance.h"

namespace eacp::Graphics
{
// There is no system-wide answer to ask for. The nearest thing is the freedesktop
// appearance setting on xdg-desktop-portal's org.freedesktop.appearance
// interface, which needs a D-Bus connection and a portal running; GTK and Qt
// each also keep their own theme preference, and none of the three is the OS's.
// Reporting light rather than guessing keeps the default the same as a machine
// with no preference set.
bool isSystemDarkMode()
{
    return false;
}
} // namespace eacp::Graphics
