#include "Clipboard.h"

namespace eacp::Clipboard
{
bool copyText(std::string_view)
{
    return false;
}

// No clipboard backend yet: there is no windowing layer here to own an X11 or
// Wayland connection. Empty is the documented answer for such a platform.
std::string getText()
{
    return {};
}

bool hasText()
{
    return false;
}

bool copyFiles(const Vector<std::string>&)
{
    return false;
}
} // namespace eacp::Clipboard
