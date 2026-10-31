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

// What the app passes to WerRegisterRuntimeExceptionModule: a block in the
// app's memory that the Windows Error Reporting helper DLL, running inside
// WerFault.exe, reads with ReadProcessMemory. The DLL writes the exception
// into it, and into the shared crash fields at the addresses it holds, just
// as the in-app handler would, so the handler process reports it unchanged.
// Plain Windows types only: the DLL links nothing of eacp.
namespace eacp::CrashReporting::Detail
{
// One helper DLL can serve many eacp apps, so both sides check this layout's
// version: the DLL in each WerRegistration it reads, the app at start() through
// the DLL's exported werProtocolVersionFunctionName.
constexpr auto werRegistrationMagic = std::uint32_t {0xEAC9EE01};
constexpr auto werRegistrationVersion = std::uint32_t {1};
constexpr auto werProtocolVersionFunctionName =
    std::string_view {"eacpWindowsErrorReportingHelperProtocolVersion"};
using WerProtocolVersionFunction = std::uint32_t (*)();
constexpr auto werModuleFileName =
    std::wstring_view {L"eacp-windows-error-reporting-helper.dll"};
constexpr auto werHelperModulesKey =
    std::wstring_view {L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\"
                       L"RuntimeExceptionHelperModules"};

struct WerRegistration
{
    std::uint32_t magic;
    std::uint32_t version;

    // Handles in the app's process; the DLL copies them out with
    // DuplicateHandle.
    std::uint64_t crashEventHandle;
    std::uint64_t handlerProcessHandle;

    // Addresses, in the app, of the SharedCrashInfo fields of the same names.
    std::uint64_t codeAddress;
    std::uint64_t faultAddressAddress;
    std::uint64_t threadIdAddress;
    std::uint64_t exceptionPointersAddressAddress;

    // Written by the DLL; pointers already holds the addresses of the other two.
    EXCEPTION_POINTERS pointers;
    EXCEPTION_RECORD record;
    CONTEXT context;
};
} // namespace eacp::CrashReporting::Detail
