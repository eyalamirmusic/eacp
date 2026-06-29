#include "LoginItem.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <string>

namespace eacp::Apps
{
namespace
{
constexpr auto* runKeyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring executablePath()
{
    auto buffer = std::wstring(MAX_PATH, L'\0');

    while (true)
    {
        auto length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

        if (length == 0)
            return {};

        if (length < buffer.size() - 1)
        {
            buffer.resize(length);
            return buffer;
        }

        buffer.resize(buffer.size() * 2);
    }
}

std::wstring valueNameForPath(const std::wstring& path)
{
    auto slash = path.find_last_of(L"\\/");
    auto name = slash == std::wstring::npos ? path : path.substr(slash + 1);

    if (name.empty())
        return L"eacp";

    return name;
}

std::wstring runCommandForPath(const std::wstring& path)
{
    return L"\"" + path + L"\"";
}
} // namespace

void setLaunchAtLogin(bool enabled)
{
    auto path = executablePath();
    if (path.empty())
        return;

    auto valueName = valueNameForPath(path);
    HKEY key = nullptr;

    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        runKeyPath,
                        0,
                        nullptr,
                        0,
                        KEY_SET_VALUE,
                        nullptr,
                        &key,
                        nullptr)
        != ERROR_SUCCESS)
        return;

    if (enabled)
    {
        auto command = runCommandForPath(path);
        RegSetValueExW(key,
                       valueName.c_str(),
                       0,
                       REG_SZ,
                       reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1)
                                           * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, valueName.c_str());
    }

    RegCloseKey(key);
}

bool isLaunchAtLogin()
{
    auto path = executablePath();
    if (path.empty())
        return false;

    auto valueName = valueNameForPath(path);
    HKEY key = nullptr;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, runKeyPath, 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS)
        return false;

    auto expected = runCommandForPath(path);
    auto bytes = DWORD {};

    auto status = RegGetValueW(key,
                               nullptr,
                               valueName.c_str(),
                               RRF_RT_REG_SZ,
                               nullptr,
                               nullptr,
                               &bytes);

    if (status != ERROR_SUCCESS || bytes == 0)
    {
        RegCloseKey(key);
        return false;
    }

    auto value = std::wstring(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(key,
                          nullptr,
                          valueName.c_str(),
                          RRF_RT_REG_SZ,
                          nullptr,
                          value.data(),
                          &bytes);
    RegCloseKey(key);

    if (status != ERROR_SUCCESS)
        return false;

    if (!value.empty() && value.back() == L'\0')
        value.pop_back();

    return value == expected;
}

} // namespace eacp::Apps
