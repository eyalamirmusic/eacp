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

// An empty value is how the CRT spells a removal: _putenv_s deletes the
// variable rather than storing an empty one, so getEnv answers nullopt after
// this exactly as it does on POSIX.
void unsetEnv(std::string_view name)
{
    _putenv_s(std::string {name}.c_str(), "");
}

} // namespace eacp
