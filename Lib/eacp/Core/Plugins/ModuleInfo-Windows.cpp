#include "ModuleInfo.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace eacp::Plugins
{
void* getCurrentModuleHandle()
{
    auto module = HMODULE {};

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) &getCurrentModuleHandle,
                       &module);

    return module;
}

bool isDynamicLibrary()
{
    static const auto result =
        getCurrentModuleHandle() != (void*) GetModuleHandleW(nullptr);

    return result;
}

FilePath getCurrentModulePath()
{
    auto buffer = std::wstring(MAX_PATH, L'\0');
    auto length = GetModuleFileNameW(
        (HMODULE) getCurrentModuleHandle(), buffer.data(), (DWORD) buffer.size());

    if (length == 0)
        return {};

    buffer.resize(length);

    auto utf8Length = WideCharToMultiByte(CP_UTF8,
                                          0,
                                          buffer.data(),
                                          (int) buffer.size(),
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr);

    auto result = std::string((std::size_t) utf8Length, '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        buffer.data(),
                        (int) buffer.size(),
                        result.data(),
                        utf8Length,
                        nullptr,
                        nullptr);

    for (auto& character: result)
        if (character == '\\')
            character = '/';

    return FilePath {std::move(result)};
}

std::string getModuleIdentitySuffix()
{
    auto value = (std::uintptr_t) getCurrentModuleHandle();
    auto result = std::string();

    while (value != 0)
    {
        result += "0123456789abcdef"[value & 0xf];
        value >>= 4;
    }

    return result;
}

std::wstring getUniqueWindowClassName(const wchar_t* root)
{
    auto suffix = getModuleIdentitySuffix();
    auto result = std::wstring(root) + L"_";

    for (auto c: suffix)
        result += (wchar_t) c;

    return result;
}
} // namespace eacp::Plugins
