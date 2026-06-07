#include "Environment.h"

#include <cstdlib>

namespace eacp
{

std::optional<std::string> getEnv(std::string_view name)
{
    char* value = nullptr;
    auto size = std::size_t {0};
    if (_dupenv_s(&value, &size, std::string {name}.c_str()) != 0
        || value == nullptr)
        return std::nullopt;

    auto result = std::string {value};
    std::free(value);
    return result;
}

} // namespace eacp
