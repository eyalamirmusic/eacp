#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <string>
#include <string_view>

namespace eacp::Clipboard
{
bool copyText(std::string_view text);
bool copyFiles(const Vector<std::string>& paths);
} // namespace eacp::Clipboard
