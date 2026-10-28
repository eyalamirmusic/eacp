#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <string_view>

// What the app hands WerRegisterRuntimeExceptionModule as its context: an
// address in the app that the module, running in WerFault.exe, reads with
// ReadProcessMemory. The module writes the exception into it and the shared
// block's crash fields at the addresses it names, exactly as the in-app filter
// would, so the handler takes it from there unchanged. Plain Windows types
// only: the module links nothing of eacp.
namespace eacp::CrashReporting::Detail
{
constexpr auto werRegistrationMagic = std::uint32_t {0xEAC9EE01};
constexpr auto werRegistrationVersion = std::uint32_t {1};
constexpr auto werModuleFileName = std::wstring_view {L"eacp-crash-wer.dll"};
constexpr auto werHelperModulesKey =
    std::wstring_view {L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\"
                       L"RuntimeExceptionHelperModules"};

struct WerRegistration
{
    std::uint32_t magic;
    std::uint32_t version;

    // The app's own handles, which the module duplicates out of it.
    std::uint64_t crashEventHandle;
    std::uint64_t handlerProcessHandle;

    // Where SharedCrashInfo's fields of these names are in the app.
    std::uint64_t codeAddress;
    std::uint64_t faultAddressAddress;
    std::uint64_t threadIdAddress;
    std::uint64_t exceptionPointersAddressAddress;

    // Filled by the module; pointers already points at the other two.
    EXCEPTION_POINTERS pointers;
    EXCEPTION_RECORD record;
    CONTEXT context;
};
} // namespace eacp::CrashReporting::Detail
