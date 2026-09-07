#include "SystemAppearance.h"

namespace eacp::Graphics
{
// No system-wide setting to ask for without a D-Bus portal.
bool isSystemDarkMode()
{
    return false;
}
} // namespace eacp::Graphics
