#pragma once

#include <filesystem>
#include <string>

namespace eacp::Files
{
std::string readFile(const std::string& path);
std::string getBundleResourcePath(const std::string& filename);
std::string filenameFromPath(const std::string& path);
bool isUnderRoot(const std::filesystem::path& file,
                 const std::filesystem::path& root);
} // namespace eacp::Files
