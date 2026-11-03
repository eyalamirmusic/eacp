#include "Environment.h"
#include "Environment-Posix.h"

#include <cstdlib>
#include <mutex>

extern char** environ;

namespace eacp
{
namespace
{
// setenv and unsetenv may reallocate environ, so a thread walking it while
// another writes reads freed memory. Every access eacp makes goes through this.
std::mutex& getEnvironmentMutex()
{
    static auto mutex = std::mutex {};
    return mutex;
}
} // namespace

std::optional<std::string> getEnv(std::string_view name)
{
    auto lock = std::lock_guard {getEnvironmentMutex()};
    const auto* value = std::getenv(std::string {name}.c_str());
    if (value == nullptr)
        return std::nullopt;
    return std::string {value};
}

void setEnv(std::string_view name, std::string_view value)
{
    auto lock = std::lock_guard {getEnvironmentMutex()};
    setenv(std::string {name}.c_str(), std::string {value}.c_str(), 1);
}

void unsetEnv(std::string_view name)
{
    auto lock = std::lock_guard {getEnvironmentMutex()};
    unsetenv(std::string {name}.c_str());
}

Vector<std::string> copyEnvironmentEntries()
{
    auto lock = std::lock_guard {getEnvironmentMutex()};
    auto entries = Vector<std::string> {};

    for (auto entry = environ; *entry != nullptr; ++entry)
        entries.push_back(*entry);

    return entries;
}

} // namespace eacp
