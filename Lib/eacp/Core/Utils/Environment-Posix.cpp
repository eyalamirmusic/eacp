#include "Environment.h"

#include <cstdlib>

namespace eacp
{

std::optional<std::string> getEnv(std::string_view name)
{
    const auto* value = std::getenv(std::string {name}.c_str());
    if (value == nullptr)
        return std::nullopt;
    return std::string {value};
}

void setEnv(std::string_view name, std::string_view value)
{
    setenv(std::string {name}.c_str(), std::string {value}.c_str(), 1);
}

} // namespace eacp
