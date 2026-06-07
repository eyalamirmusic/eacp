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

} // namespace eacp
