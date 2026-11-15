#include "WinInclude.h"

#include "Environment.h"

#include <cstdlib>

// Windows keeps two environments: the process's block, which
// GetEnvironmentVariableW reads and every child inherits, and the C runtime's
// copy, which getenv reads and which is narrow, in the ANSI code page. getEnv
// reads the block, as UTF-8. setEnv and unsetEnv go through _wputenv_s alone:
// the UCRT updates its wide and narrow tables and the block together, under its
// own environment lock, so two threads setting one variable cannot leave the
// block and the copy disagreeing, which a second SetEnvironmentVariableW call
// outside that lock could.
namespace eacp
{

std::optional<std::string> getEnv(std::string_view name)
{
    const auto wideName = Strings::widen(name);
    auto value = std::wstring(128, L'\0');

    while (true)
    {
        SetLastError(ERROR_SUCCESS);
        const auto length = GetEnvironmentVariableW(
            wideName.c_str(), value.data(), static_cast<DWORD>(value.size()));

        if (length == 0)
        {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
                return std::nullopt;

            return std::string {};
        }

        if (length < value.size())
        {
            value.resize(length);
            return Strings::narrow(value);
        }

        value.resize(length);
    }
}

// An empty value is how the CRT spells a removal: _wputenv_s deletes the
// variable rather than storing an empty one, from the block too, so getEnv
// answers nullopt after this exactly as it did before.
void setEnv(std::string_view name, std::string_view value)
{
    _wputenv_s(Strings::widen(name).c_str(), Strings::widen(value).c_str());
}

void unsetEnv(std::string_view name)
{
    setEnv(name, {});
}

} // namespace eacp
