#include "SystemAppearance.h"

#include "../Window/WebWindowSystem-Web.h"

namespace eacp::Graphics
{
// The page's prefers-color-scheme; false under node.
bool isSystemDarkMode()
{
    return eacp_web_prefers_dark_scheme() != 0;
}
} // namespace eacp::Graphics
