#pragma once

#include "Clipboard.h"

// Linux has no clipboard without a windowing connection, and eacp-core links
// neither Wayland nor X11. So Core holds the hooks and whichever windowing
// backend comes up fills them in, the way Threads::addLoopSource lets Graphics
// pump a Wayland connection on Core's loop.

namespace eacp::Clipboard
{
// Every hook is called on the message thread, and defaults to the answer a
// machine with no clipboard gives.
struct Backend
{
    std::function<bool(std::string_view)> copyText = [](std::string_view)
    { return false; };

    std::function<bool(const Vector<std::string>&)> copyFiles =
        [](const Vector<std::string>&) { return false; };

    std::function<std::string()> getText = [] { return std::string {}; };
    std::function<bool()> hasText = [] { return false; };
};

// Installed when the compositor connection and its seat come up, and cleared
// on the way down: the hooks must not outlive the connection they close over.
void setBackend(Backend backend);
void clearBackend();
} // namespace eacp::Clipboard
