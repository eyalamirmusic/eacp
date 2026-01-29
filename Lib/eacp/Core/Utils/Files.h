#pragma once

#include <string>

namespace eacp::Files
{
std::string readFile(const std::string& path);
std::string getBundleResourcePath(const std::string& filename);
} // namespace eacp::Files
