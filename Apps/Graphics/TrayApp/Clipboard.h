#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <string>

namespace TrayAppNative
{
bool copyFilesToClipboard(const EA::Vector<std::string>& paths);
}
