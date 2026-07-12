#pragma once

#include "Common.h"

namespace eacp
{

// Reads an environment variable, or nullopt when it is unset. Implemented per
// platform (Environment-Windows.cpp uses _dupenv_s to avoid the deprecated
// getenv on MSVC; Environment-Posix.cpp uses std::getenv).
std::optional<std::string> getEnv(std::string_view name);

// Reads an environment variable, or an empty string when it is unset, for
// callers that treat "unset" and "empty" the same.
std::string getEnvValue(std::string_view name);

// Sets (or overwrites) an environment variable for this process.
void setEnv(std::string_view name, std::string_view value);

} // namespace eacp
