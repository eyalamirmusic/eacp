#include "Environment.h"

#include <eacp/Core/Platform/Platform.h>

namespace eacp
{

std::filesystem::path homeDirectory()
{
    auto variable = Platform::isWindows() ? "USERPROFILE" : "HOME";
    auto value = getEnv(variable);
    return value ? std::filesystem::path {*value} : std::filesystem::path {};
}

} // namespace eacp
