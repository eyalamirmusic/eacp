#include "../WindowsErrorReportingRegistration-Windows.h"

#include <werapi.h>

#include <array>
#include <cstddef>

// The Windows Error Reporting (WER) helper DLL. WerFault.exe, which Windows
// runs when a process dies, loads it for a crash no in-app handler saw
// (__fastfail, heap corruption). It hands the crash to that app's eacp
// handler process and tells Windows the crash is taken, so Windows files none.
using namespace eacp::CrashReporting::Detail;

namespace
{
constexpr auto handlerCaptureTimeoutMilliseconds = DWORD {60000};

template <typename T>
bool readFromProcess(HANDLE process, std::uint64_t address, T& value)
{
    auto bytesRead = SIZE_T {0};
    auto* source = reinterpret_cast<const void*>(address);
    return ::ReadProcessMemory(process, source, &value, sizeof(T), &bytesRead)
           && bytesRead == sizeof(T);
}

template <typename T>
bool writeToProcess(HANDLE process, std::uint64_t address, const T& value)
{
    auto bytesWritten = SIZE_T {0};
    auto* destination = reinterpret_cast<void*>(address);
    return ::WriteProcessMemory(
               process, destination, &value, sizeof(T), &bytesWritten)
           && bytesWritten == sizeof(T);
}

HANDLE duplicateFromProcess(HANDLE process, std::uint64_t handle, DWORD access)
{
    auto duplicate = HANDLE {nullptr};
    auto* source = reinterpret_cast<HANDLE>(handle);

    if (!::DuplicateHandle(
            process, source, ::GetCurrentProcess(), &duplicate, access, FALSE, 0))
        return nullptr;

    return duplicate;
}

bool isRegistrationValid(const WerRegistration& registration)
{
    return registration.magic == werRegistrationMagic
           && registration.version == werRegistrationVersion;
}

// An exception code already written means the in-app handler caught this crash
// and is reporting it itself.
bool isCrashAlreadyRecorded(HANDLE process, const WerRegistration& registration)
{
    auto code = std::uint64_t {0};
    return !readFromProcess(process, registration.codeAddress, code) || code != 0;
}

bool recordCrash(const WER_RUNTIME_EXCEPTION_INFORMATION& information,
                 std::uint64_t registrationAddress,
                 const WerRegistration& registration)
{
    auto process = information.hProcess;
    const auto& record = information.exceptionRecord;
    auto recordAddress = registrationAddress + offsetof(WerRegistration, record);
    auto contextAddress = registrationAddress + offsetof(WerRegistration, context);
    auto pointersAddress = registrationAddress + offsetof(WerRegistration, pointers);
    auto faultAddress =
        std::uint64_t {reinterpret_cast<std::uintptr_t>(record.ExceptionAddress)};
    auto threadId = std::uint64_t {::GetThreadId(information.hThread)};
    auto code = std::uint64_t {record.ExceptionCode};

    // The exception code is written last: the handler process starts reading
    // the crash as soon as the code appears.
    return writeToProcess(process, recordAddress, record)
           && writeToProcess(process, contextAddress, information.context)
           && writeToProcess(process, registration.faultAddressAddress, faultAddress)
           && writeToProcess(process, registration.threadIdAddress, threadId)
           && writeToProcess(process,
                             registration.exceptionPointersAddressAddress,
                             pointersAddress)
           && writeToProcess(process, registration.codeAddress, code);
}

// Waits for the app to end, not the handler: the handler ends the app once the
// report is written, then keeps running to send it.
bool handOverToHandler(const WER_RUNTIME_EXCEPTION_INFORMATION& information,
                       std::uint64_t registrationAddress)
{
    auto process = information.hProcess;
    auto registration = WerRegistration {};

    if (!readFromProcess(process, registrationAddress, registration)
        || !isRegistrationValid(registration)
        || isCrashAlreadyRecorded(process, registration))
        return false;

    auto handler = duplicateFromProcess(
        process, registration.handlerProcessHandle, SYNCHRONIZE);
    auto crashEvent = duplicateFromProcess(
        process, registration.crashEventHandle, EVENT_MODIFY_STATE);
    auto isHandlerRunning =
        handler != nullptr && ::WaitForSingleObject(handler, 0) == WAIT_TIMEOUT;
    auto handedOver = isHandlerRunning && crashEvent != nullptr
                      && recordCrash(information, registrationAddress, registration)
                      && ::SetEvent(crashEvent);

    if (handedOver)
    {
        auto waitables = std::to_array<HANDLE>({process, handler});
        ::WaitForMultipleObjects(static_cast<DWORD>(waitables.size()),
                                 waitables.data(),
                                 FALSE,
                                 handlerCaptureTimeoutMilliseconds);
    }

    for (auto handle: {handler, crashEvent})
    {
        if (handle != nullptr)
            ::CloseHandle(handle);
    }

    return handedOver;
}
} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI OutOfProcessExceptionEventCallback(
    PVOID context,
    const PWER_RUNTIME_EXCEPTION_INFORMATION information,
    BOOL* ownershipClaimed,
    PWSTR,
    PDWORD,
    PDWORD)
{
    auto registrationAddress =
        std::uint64_t {reinterpret_cast<std::uintptr_t>(context)};
    *ownershipClaimed = handOverToHandler(*information, registrationAddress);
    return S_OK;
}

// Windows calls this only for a crash this DLL took, and files no report of
// its own for it.
extern "C" __declspec(dllexport) HRESULT WINAPI
    OutOfProcessExceptionEventSignatureCallback(
        PVOID,
        const PWER_RUNTIME_EXCEPTION_INFORMATION,
        DWORD,
        PWSTR,
        PDWORD,
        PWSTR,
        PDWORD)
{
    return E_NOTIMPL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI
    OutOfProcessExceptionEventDebuggerLaunchCallback(
        PVOID,
        const PWER_RUNTIME_EXCEPTION_INFORMATION,
        PBOOL isCustomDebugger,
        PWSTR,
        PDWORD,
        PBOOL isDebuggerAutolaunch)
{
    *isCustomDebugger = FALSE;
    *isDebuggerAutolaunch = FALSE;
    return E_NOTIMPL;
}
