#include "Clipboard.h"

namespace eacp::Clipboard
{
bool copyText(std::string_view)
{
    return false;
}

// Linux has no clipboard backend yet: there is no windowing layer here to own
// an X11 or Wayland connection, and the clipboard requires one. Empty is the
// documented answer for a platform without a clipboard, so callers need no
// special case.
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
