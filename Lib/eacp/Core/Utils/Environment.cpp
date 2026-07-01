#include "Environment.h"

#include <eacp/Core/Platform/Platform.h>

namespace eacp
{

std::string getEnvValue(std::string_view name)
{
    return getEnv(name).value_or(std::string {});
}

std::filesystem::path homeDirectory()
{
    auto variable = Platform::isWindows() ? "USERPROFILE" : "HOME";
    return std::filesystem::path {getEnvValue(variable)};
}

} // namespace eacp
