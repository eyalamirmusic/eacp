#include "Common.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/WinInclude.h>

#include <cstdlib>

using namespace nano;

// Windows keeps two environments: the process's own block, which
// GetEnvironmentVariableW reads and every child inherits, and the C runtime's
// copy that getenv reads. setEnv and getEnv must use the first as UTF-8, and
// keep the second in step for code that reads it.
namespace EnvironmentWindowsTests
{
namespace
{
constexpr auto variableName = "EACP_ENVIRONMENT_WINDOWS_TEST";

// U+00DC n U+00EF c U+00F8 d U+00E9 - U+65E5 U+672C as UTF-8.
std::string nonAsciiValue()
{
    const auto text = std::u8string {u8"\u00dcn\u00efc\u00f8d\u00e9-\u65e5\u672c"};
    return {text.begin(), text.end()};
}

std::optional<std::wstring> blockValue(const char* name)
{
    auto value = std::wstring(256, L'\0');
    const auto length = GetEnvironmentVariableW(
        eacp::Strings::widen(name).c_str(), value.data(), (DWORD) value.size());

    if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        return std::nullopt;

    value.resize(length);
    return value;
}

std::optional<std::string> runtimeValue(const char* name)
{
    auto value = std::string(256, '\0');
    auto length = std::size_t {0};

    if (getenv_s(&length, value.data(), value.size(), name) != 0 || length == 0)
        return std::nullopt;

    value.resize(length - 1);
    return value;
}
} // namespace

auto tSetEnvWritesTheBlockAsUtf8 =
    test("Environment/setEnv writes the process block as UTF-8") = []
{
    eacp::setEnv(variableName, nonAsciiValue());

    check(blockValue(variableName) == eacp::Strings::widen(nonAsciiValue()));
    check(eacp::getEnv(variableName) == nonAsciiValue());

    eacp::unsetEnv(variableName);
};

auto tGetEnvReadsTheBlock =
    test("Environment/getEnv reads a value set in the process block") = []
{
    SetEnvironmentVariableW(eacp::Strings::widen(variableName).c_str(),
                            eacp::Strings::widen(nonAsciiValue()).c_str());

    check(eacp::getEnv(variableName) == nonAsciiValue());

    SetEnvironmentVariableW(eacp::Strings::widen(variableName).c_str(), nullptr);
    check(!eacp::getEnv(variableName).has_value());
};

// The block can hold a variable with an empty value (a parent can pass one, or
// SetEnvironmentVariableW can set one), which GetEnvironmentVariableW answers
// with 0 and no error: present, and empty, not unset.
auto tEmptyIsPresent =
    test("Environment/getEnv reads a present but empty variable as empty") = []
{
    SetEnvironmentVariableW(eacp::Strings::widen(variableName).c_str(), L"");

    check(eacp::getEnv(variableName) == std::string {});

    SetEnvironmentVariableW(eacp::Strings::widen(variableName).c_str(), nullptr);
};

auto tRuntimeCopyFollows =
    test("Environment/setEnv and unsetEnv keep the C runtime's copy in step") = []
{
    eacp::setEnv(variableName, "1");
    check(runtimeValue(variableName) == std::string {"1"});

    eacp::unsetEnv(variableName);
    check(!runtimeValue(variableName).has_value());
    check(!blockValue(variableName).has_value());
    check(!eacp::getEnv(variableName).has_value());
};

auto tLongValue = test("Environment/getEnv reads a value longer than 256") = []
{
    const auto value = std::string(1000, 'x') + nonAsciiValue();
    eacp::setEnv(variableName, value);

    check(eacp::getEnv(variableName) == value);

    eacp::unsetEnv(variableName);
};
} // namespace EnvironmentWindowsTests
