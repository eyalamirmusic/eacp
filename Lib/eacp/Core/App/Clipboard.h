#pragma once

#include "../Utils/Common.h"

namespace eacp::Clipboard
{
bool copyText(std::string_view text);
bool copyFiles(const Vector<std::string>& paths);

// Empty when the clipboard holds no text, and on platforms with no clipboard.
// Reads the system clipboard on every call rather than caching.
std::string getText();

// Whether getText would return anything, without pulling the payload across.
bool hasText();
} // namespace eacp::Clipboard
