#pragma once

#include "../Utils/Common.h"

namespace eacp::Clipboard
{
bool copyText(std::string_view text);
bool copyFiles(const Vector<std::string>& paths);

// The clipboard's current plain-text content, or an empty string when it
// holds none (or the platform has no readable clipboard).
std::string getText();
} // namespace eacp::Clipboard
