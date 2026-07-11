#pragma once

#include "Common.h"

namespace eacp::Files
{
std::string readFile(const std::string& path);
std::string getBundleResourcePath(const std::string& filename);
std::string filenameFromPath(const std::string& path);
} // namespace eacp::Files
