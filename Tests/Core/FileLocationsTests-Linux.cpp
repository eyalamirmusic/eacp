#include "Common.h"

#include <eacp/Core/Utils/Environment.h>

#include <pwd.h>
#include <unistd.h>

using namespace nano;

namespace FileLocationsLinuxTests
{
using eacp::FilePath;

// Sets an environment variable, or removes it, for the length of a scope and
// puts back what was there.
struct ScopedVariable
{
    ScopedVariable(std::string_view nameToUse, std::optional<std::string_view> value)
        : name(nameToUse)
        , previous(eacp::getEnv(nameToUse))
    {
        if (value.has_value())
            eacp::setEnv(name, *value);
        else
            eacp::unsetEnv(name);
    }

    ~ScopedVariable()
    {
        if (previous.has_value())
            eacp::setEnv(name, *previous);
        else
            eacp::unsetEnv(name);
    }

    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

    std::string name;
    std::optional<std::string> previous;
};

std::string passwordDatabaseHomeForTest()
{
    auto* entry = ::getpwuid(::getuid());
    return entry != nullptr && entry->pw_dir != nullptr ? entry->pw_dir : "";
}

auto tHomeTrailingSlash = test("FileLocationsLinux/homeDropsATrailingSlash") = []
{
    {
        auto home = ScopedVariable {"HOME", "/home/someone/"};
        check(FilePath::homeDirectory().str() == "/home/someone");
    }

    {
        auto home = ScopedVariable {"HOME", "/home/someone"};
        check(FilePath::homeDirectory().str() == "/home/someone");
    }

    auto root = ScopedVariable {"HOME", "/"};
    check(FilePath::homeDirectory().str() == "/");
};

auto tHomeFallback = test("FileLocationsLinux/homeFallsBackToPasswordDatabase") = []
{
    auto expected = passwordDatabaseHomeForTest();

    if (expected.size() > 1 && expected.ends_with('/'))
        expected.pop_back();

    {
        auto home = ScopedVariable {"HOME", std::nullopt};
        check(FilePath::homeDirectory().str() == expected);
    }

    auto empty = ScopedVariable {"HOME", ""};
    check(FilePath::homeDirectory().str() == expected);
};
} // namespace FileLocationsLinuxTests
