#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace eacp
{

// Reads an environment variable, or nullopt when it is unset. Implemented per
// platform (Environment-Windows.cpp uses _dupenv_s to avoid the deprecated
// getenv on MSVC; Environment-Posix.cpp uses std::getenv).
std::optional<std::string> getEnv(std::string_view name);

// The current user's home directory, or an empty path when it can't be
// resolved. Reads USERPROFILE on Windows and HOME elsewhere.
std::filesystem::path homeDirectory();

} // namespace eacp
