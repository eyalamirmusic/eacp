#include "CrashChannel-Windows.h"
#include "CppExceptions-Windows.h"
#include "CrashReportingPlatform.h"
#include "ExceptionNames-Windows.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <array>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <intrin.h>
#include <optional>
#include <typeinfo>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto handlerCaptureTimeoutMilliseconds = DWORD {60000};
constexpr auto handlerStopTimeoutMilliseconds = DWORD {5000};
constexpr auto stackOverflowReserveBytes = ULONG {64 * 1024};
constexpr auto terminateReason = std::string_view {"std::terminate"};
constexpr auto unknownExceptionType = std::string_view {"unknown exception"};
constexpr auto messageSeparator = std::string_view {": "};
constexpr auto abortReason = std::string_view {"SIGABRT"};
constexpr auto pureVirtualReason = std::string_view {"purecall"};
constexpr auto invalidParameterReason = std::string_view {"invalid parameter"};
constexpr auto reportedCrashExitCode = UINT {3};

// A code of eacp's own for a crash that is not a structured exception, so the
// handler has an EXCEPTION_POINTERS to walk for abort() and its kind too.
constexpr auto softwareCrashExceptionCode = DWORD {0xE0EAC000};

struct WindowsCrashClient
{
    std::optional<std::reference_wrapper<SharedState>> state;
    HANDLE sharedMapping = nullptr;
    HANDLE crashEvent = nullptr;
    HANDLE handlerProcess = nullptr;
    std::atomic<bool> isCrashing {false};

    LPTOP_LEVEL_EXCEPTION_FILTER previousExceptionFilter = nullptr;
    _invalid_parameter_handler previousInvalidParameterHandler = nullptr;
    _purecall_handler previousPureCallHandler = nullptr;
    _crt_signal_t previousAbortHandler = SIG_DFL;
    std::terminate_handler previousTerminateHandler = nullptr;
};

WindowsCrashClient& getWindowsCrashClient()
{
    static auto client = WindowsCrashClient {};
    return client;
}
bool hasRecordedReason(const SharedCrashInfo& crash)
{
    return crash.reason[0] != '\0';
}

void recordReason(SharedCrashInfo& crash, std::string_view reason)
{
    if (!hasRecordedReason(crash))
        copyBoundedText(crash.reason, reason);
}

std::uint64_t getFaultAddress(const EXCEPTION_RECORD& record)
{
    auto isAccessViolation = record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION
                             && record.NumberParameters >= 2;

    if (isAccessViolation)
        return record.ExceptionInformation[1];

    return reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
}

void recordException(SharedCrashInfo& crash, EXCEPTION_POINTERS& pointers)
{
    const auto& record = *pointers.ExceptionRecord;

    if (auto name = findExceptionName(record.ExceptionCode))
        recordReason(crash, *name);
    else
        recordReason(crash, "unknown exception");

    crash.code = record.ExceptionCode;
    crash.faultAddress = getFaultAddress(record);
    crash.threadId = ::GetCurrentThreadId();
    crash.exceptionPointersAddress = reinterpret_cast<std::uintptr_t>(&pointers);
}

// Hands the crash to the handler, which writes the report and ends this
// process; returns only when there is no handler or it never did.
void handOverToHandler(EXCEPTION_POINTERS& pointers)
{
    auto& client = getWindowsCrashClient();

    if (!client.state || client.isCrashing.exchange(true))
        return;

    auto& state = client.state->get();
    recordException(state.crash, pointers);
    state.tableSequence.fetch_add(2, std::memory_order_release);

    if (client.handlerProcess == nullptr || client.crashEvent == nullptr)
        return;

    ::AllowSetForegroundWindow(::GetProcessId(client.handlerProcess));
    ::SetEvent(client.crashEvent);
    auto waitables = std::to_array<HANDLE>({client.handlerProcess});
    ::WaitForMultipleObjects(static_cast<DWORD>(waitables.size()),
                             waitables.data(),
                             FALSE,
                             handlerCaptureTimeoutMilliseconds);
}

// abort(), a pure virtual call and the CRT's invalid-parameter check raise no
// exception of their own, so one is made here, with this thread's context.
[[noreturn]] void reportSoftwareCrash(std::string_view reason)
{
    auto& client = getWindowsCrashClient();

    if (client.state)
        recordReason(client.state->get().crash, reason);

    auto context = CONTEXT {};
    ::RtlCaptureContext(&context);

    auto record = EXCEPTION_RECORD {};
    record.ExceptionCode = softwareCrashExceptionCode;
    record.ExceptionAddress = _ReturnAddress();

    auto pointers = EXCEPTION_POINTERS {&record, &context};
    handOverToHandler(pointers);
    ::TerminateProcess(::GetCurrentProcess(), reportedCrashExitCode);
    std::abort();
}

void handleAbortSignal(int)
{
    reportSoftwareCrash(abortReason);
}

void handlePureVirtualCall()
{
    reportSoftwareCrash(pureVirtualReason);
}

void handleInvalidParameter(
    const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    reportSoftwareCrash(invalidParameterReason);
}

void appendBoundedText(BoundedText<maxCrashMessageLength>& destination,
                       std::size_t& length,
                       std::string_view text)
{
    auto available = destination.size() - 1 - length;
    auto count = std::min(text.size(), available);
    std::copy_n(text.data(), count, destination.data() + length);
    length += count;
    destination[length] = '\0';
}

void recordTerminateMessage(std::string_view name, std::string_view detail)
{
    auto& client = getWindowsCrashClient();

    if (!client.state || hasRecordedReason(client.state->get().crash))
        return;

    auto& crash = client.state->get().crash;
    copyBoundedText(crash.reason, terminateReason);

    auto length = std::size_t {0};
    appendBoundedText(crash.message, length, name);

    if (!name.empty() && !detail.empty())
        appendBoundedText(crash.message, length, messageSeparator);

    appendBoundedText(crash.message, length, detail);
}

LONG WINAPI handleUnhandledException(EXCEPTION_POINTERS* pointers)
{
    if (isCppException(*pointers->ExceptionRecord))
        readCppException(*pointers->ExceptionRecord, &recordTerminateMessage);

    handOverToHandler(*pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

std::string_view getExceptionTypeName(const std::type_info& type)
{
    auto name = std::string_view {type.name()};
    constexpr auto classPrefix = std::string_view {"class "};

    if (name.starts_with(classPrefix))
        name.remove_prefix(classPrefix.size());

    return name;
}

void recordCurrentException()
{
    auto exception = std::current_exception();

    if (!exception)
    {
        recordTerminateMessage({}, {});
        return;
    }

    try
    {
        std::rethrow_exception(exception);
    }
    catch (const std::exception& error)
    {
        recordTerminateMessage(getExceptionTypeName(typeid(error)), error.what());
    }
    catch (...)
    {
        recordTerminateMessage(unknownExceptionType, {});
    }
}

[[noreturn]] void handleTerminate()
{
    recordCurrentException();
    std::abort();
}

void reserveStackForOverflowReport()
{
    auto reserve = stackOverflowReserveBytes;
    ::SetThreadStackGuarantee(&reserve);
}

std::wstring quoteCommandLineArgument(const std::wstring& argument)
{
    auto quoted = std::wstring {L"\""};
    auto pendingBackslashes = std::size_t {0};

    for (auto character: argument)
    {
        if (character == L'\\')
        {
            ++pendingBackslashes;
            continue;
        }

        if (character == L'"')
            quoted.append(pendingBackslashes * 2 + 1, L'\\');
        else
            quoted.append(pendingBackslashes, L'\\');

        pendingBackslashes = 0;
        quoted += character;
    }

    quoted.append(pendingBackslashes * 2, L'\\');
    return quoted + L"\"";
}

std::wstring makeWindowsCommandLine(const Vector<std::string>& arguments)
{
    auto commandLine = std::wstring {};

    for (const auto& argument: arguments)
    {
        if (!commandLine.empty())
            commandLine += L' ';

        commandLine += quoteCommandLineArgument(Strings::widen(argument));
    }

    return commandLine;
}

void* mapSharedStateView(HANDLE mapping)
{
    if (mapping == nullptr)
        return nullptr;

    return ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
}

HANDLE createChannelEvent(std::string_view channel, std::string_view suffix)
{
    auto name = makeChannelObjectName(channel, suffix);
    return ::CreateEventW(nullptr, FALSE, FALSE, name.c_str());
}

} // namespace
SharedState& createSharedState(const HandlerLaunch& launch)
{
    auto& client = getWindowsCrashClient();
    auto name = makeChannelObjectName(launch.channelName, sharedStateObjectSuffix);
    auto size = static_cast<DWORD>(sizeof(SharedState));
    client.sharedMapping = ::CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name.c_str());

    auto* view = mapSharedStateView(client.sharedMapping);

    if (view == nullptr)
    {
        static auto privateState = SharedState {};
        initialiseSharedState(privateState, getCurrentProcessId());
        return privateState;
    }

    auto& state = *static_cast<SharedState*>(view);
    initialiseSharedState(state, getCurrentProcessId());
    return state;
}

bool launchHandlerProcess(const HandlerLaunch& launch, SharedState&)
{
    auto& client = getWindowsCrashClient();
    client.crashEvent = createChannelEvent(launch.channelName, crashEventSuffix);
    auto readyEvent = createChannelEvent(launch.channelName, readyEventSuffix);

    if (client.sharedMapping == nullptr || client.crashEvent == nullptr
        || readyEvent == nullptr)
        return false;

    auto commandLine =
        makeWindowsCommandLine(makeHandlerCommandLine(launch, launch.channelName));
    auto executable = launch.executable.wide();
    auto startup = STARTUPINFOW {};
    startup.cb = sizeof(startup);
    auto processInfo = PROCESS_INFORMATION {};

    auto created = ::CreateProcessW(executable.c_str(),
                                    commandLine.data(),
                                    nullptr,
                                    nullptr,
                                    FALSE,
                                    0,
                                    nullptr,
                                    nullptr,
                                    &startup,
                                    &processInfo);

    if (!created)
    {
        ::CloseHandle(readyEvent);
        return false;
    }

    ::CloseHandle(processInfo.hThread);
    ::AllowSetForegroundWindow(processInfo.dwProcessId);
    client.handlerProcess = processInfo.hProcess;

    auto waitables = std::to_array<HANDLE>({readyEvent, client.handlerProcess});
    auto timeout = static_cast<DWORD>(launch.startTimeout.count());
    auto woken = ::WaitForMultipleObjects(
        static_cast<DWORD>(waitables.size()), waitables.data(), FALSE, timeout);
    ::CloseHandle(readyEvent);

    return woken == WAIT_OBJECT_0;
}

void installCrashHandlers(SharedState& state)
{
    auto& client = getWindowsCrashClient();
    client.state = state;

    reserveStackForOverflowReport();
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    client.previousExceptionFilter =
        ::SetUnhandledExceptionFilter(&handleUnhandledException);
    client.previousInvalidParameterHandler =
        _set_invalid_parameter_handler(&handleInvalidParameter);
    client.previousPureCallHandler = _set_purecall_handler(&handlePureVirtualCall);
    client.previousAbortHandler = std::signal(SIGABRT, &handleAbortSignal);
    client.previousTerminateHandler = std::set_terminate(&handleTerminate);
}

void uninstallCrashHandlers()
{
    auto& client = getWindowsCrashClient();
    ::SetUnhandledExceptionFilter(client.previousExceptionFilter);
    _set_invalid_parameter_handler(client.previousInvalidParameterHandler);
    _set_purecall_handler(client.previousPureCallHandler);
    std::signal(SIGABRT, client.previousAbortHandler);
    std::set_terminate(client.previousTerminateHandler);
}

// The crash event with stopRequested set is the handler's clean exit.
void stopHandlerProcess(SharedState& state)
{
    auto& client = getWindowsCrashClient();
    state.stopRequested.store(1, std::memory_order_release);

    if (client.crashEvent != nullptr)
        ::SetEvent(client.crashEvent);

    if (client.handlerProcess != nullptr)
    {
        ::WaitForSingleObject(client.handlerProcess, handlerStopTimeoutMilliseconds);
        ::CloseHandle(client.handlerProcess);
        client.handlerProcess = nullptr;
    }
}

bool isHandlerProcessRunning()
{
    auto& client = getWindowsCrashClient();

    if (client.handlerProcess == nullptr)
        return false;

    return ::WaitForSingleObject(client.handlerProcess, 0) == WAIT_TIMEOUT;
}

std::string getOperatingSystemVersion()
{
    auto key = HKEY {};
    auto opened = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                  L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                  0,
                                  KEY_READ,
                                  &key);

    if (opened != ERROR_SUCCESS)
        return {};

    auto build = std::array<wchar_t, maxReasonLength> {};
    auto size = static_cast<DWORD>(build.size() * sizeof(wchar_t));
    auto read = ::RegGetValueW(
        key, nullptr, L"CurrentBuild", RRF_RT_REG_SZ, nullptr, build.data(), &size);
    ::RegCloseKey(key);

    if (read != ERROR_SUCCESS)
        return {};

    return "build " + Strings::narrow(build.data());
}

std::uint64_t getCurrentProcessId()
{
    return ::GetCurrentProcessId();
}

} // namespace eacp::CrashReporting::Detail
