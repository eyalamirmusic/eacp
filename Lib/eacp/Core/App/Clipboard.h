#pragma once

#include "../Utils/Common.h"

namespace eacp::Clipboard
{
bool copyText(std::string_view text);
bool copyFiles(const Vector<std::string>& paths);
} // namespace eacp::Clipboard
