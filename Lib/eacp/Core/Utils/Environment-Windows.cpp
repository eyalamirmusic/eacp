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

void setEnv(std::string_view name, std::string_view value)
{
    _putenv_s(std::string {name}.c_str(), std::string {value}.c_str());
}

} // namespace eacp
