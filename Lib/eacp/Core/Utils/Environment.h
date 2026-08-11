#pragma once

#include "Common.h"

namespace eacp
{

// nullopt when the variable is unset.
std::optional<std::string> getEnv(std::string_view name);

// Empty when the variable is unset, for callers treating unset and empty alike.
std::string getEnvValue(std::string_view name);

void setEnv(std::string_view name, std::string_view value);

} // namespace eacp
