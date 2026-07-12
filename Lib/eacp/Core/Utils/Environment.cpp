#include "Environment.h"

#include "../Platform/Platform.h"

namespace eacp
{

std::string getEnvValue(std::string_view name)
{
    return getEnv(name).value_or(std::string {});
}

FilePath homeDirectory()
{
    auto variable = Platform::isWindows() ? "USERPROFILE" : "HOME";
    return FilePath {getEnvValue(variable)};
}

} // namespace eacp
