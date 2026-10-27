#include "CrashChannel-Windows.h"
#include "CrashReportingPlatform.h"
#include "ExceptionNames-Windows.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <dbghelp.h>

#include <array>
#include <cstdio>
#include <optional>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto maxSymbolNameLength = std::size_t {1024};
constexpr auto minidumpFileName = "minidump.dmp";
constexpr auto errorSeverityMask = DWORD {0xC0000000};
constexpr auto handlerFailedExitCode = 1;
constexpr auto appTerminationTimeoutMs = DWORD {5000};

#if defined(_M_ARM64)
constexpr auto stackWalkMachine = DWORD {IMAGE_FILE_MACHINE_ARM64};
#else
constexpr auto stackWalkMachine = DWORD {IMAGE_FILE_MACHINE_AMD64};
#endif

// Every handle the handler holds on the app, opened before any crash so a
// dying app has nothing to grant.
struct AppConnection
{
    ~AppConnection()
    {
        for (auto handle: {sharedMapping, crashEvent, appProcess})
        {
            if (handle != nullptr)
                ::CloseHandle(handle);
        }
    }

    HANDLE sharedMapping = nullptr;
    HANDLE crashEvent = nullptr;
    HANDLE appProcess = nullptr;
    std::optional<std::reference_wrapper<SharedState>> state;
};

enum class AppEvent
{
    crash,
    appExited
};

HANDLE openChannelEvent(std::string_view channel, std::string_view suffix)
{
    auto name = makeChannelObjectName(channel, suffix);
    return ::OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name.c_str());
}

void mapSharedState(AppConnection& connection, std::string_view channel)
{
    auto name = makeChannelObjectName(channel, sharedStateObjectSuffix);
    connection.sharedMapping =
        ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());

    if (connection.sharedMapping == nullptr)
        return;

    auto* view = ::MapViewOfFile(
        connection.sharedMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));

    if (view == nullptr)
        return;

    auto& state = *static_cast<SharedState*>(view);

    if (isSharedStateValid(state))
        connection.state = state;
}

bool connectToApp(AppConnection& connection, std::string_view channel)
{
    mapSharedState(connection, channel);

    if (!connection.state)
        return false;

    auto appProcessId = static_cast<DWORD>(connection.state->get().appProcessId);
    connection.appProcess = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, appProcessId);
    connection.crashEvent = openChannelEvent(channel, crashEventSuffix);

    return connection.appProcess != nullptr && connection.crashEvent != nullptr;
}

void signalHandlerReady(std::string_view channel)
{
    auto readyEvent = openChannelEvent(channel, readyEventSuffix);

    if (readyEvent == nullptr)
        return;

    ::SetEvent(readyEvent);
    ::CloseHandle(readyEvent);
}

AppEvent waitForAppEvent(const AppConnection& connection)
{
    auto waitables =
        std::to_array<HANDLE>({connection.crashEvent, connection.appProcess});
    auto woken = ::WaitForMultipleObjects(
        static_cast<DWORD>(waitables.size()), waitables.data(), FALSE, INFINITE);

    if (woken == WAIT_OBJECT_0)
        return AppEvent::crash;

    return AppEvent::appExited;
}
template <typename T>
std::optional<T> readAppMemory(HANDLE process, std::uint64_t address)
{
    auto value = T {};
    auto bytesRead = SIZE_T {0};
    auto* source = reinterpret_cast<const void*>(address);

    if (!::ReadProcessMemory(process, source, &value, sizeof(T), &bytesRead)
        || bytesRead != sizeof(T))
        return std::nullopt;

    return value;
}

std::optional<CONTEXT> readCrashContext(HANDLE process, const SharedCrashInfo& crash)
{
    auto pointers =
        readAppMemory<EXCEPTION_POINTERS>(process, crash.exceptionPointersAddress);

    if (!pointers)
        return std::nullopt;

    auto contextAddress = reinterpret_cast<std::uintptr_t>(pointers->ContextRecord);
    return readAppMemory<CONTEXT>(process, contextAddress);
}

bool writeMinidump(HANDLE process, const SharedState& state, const FilePath& path)
{
    auto file = ::CreateFileW(path.wide().c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    auto exceptionInfo = MINIDUMP_EXCEPTION_INFORMATION {};
    exceptionInfo.ThreadId = static_cast<DWORD>(state.crash.threadId);
    exceptionInfo.ExceptionPointers =
        reinterpret_cast<EXCEPTION_POINTERS*>(state.crash.exceptionPointersAddress);
    exceptionInfo.ClientPointers = TRUE;

    auto written = ::MiniDumpWriteDump(process,
                                       static_cast<DWORD>(state.appProcessId),
                                       file,
                                       MiniDumpNormal,
                                       &exceptionInfo,
                                       nullptr,
                                       nullptr);
    ::CloseHandle(file);
    return written == TRUE;
}

STACKFRAME64 makeFirstStackFrame(const CONTEXT& context)
{
    auto frame = STACKFRAME64 {};
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
#if defined(_M_ARM64)
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#else
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#endif
    return frame;
}

std::string findSymbolName(HANDLE process, std::uint64_t address)
{
    auto buffer =
        std::array<std::byte,
                   sizeof(SYMBOL_INFOW) + maxSymbolNameLength * sizeof(wchar_t)> {};
    auto& symbol = *reinterpret_cast<SYMBOL_INFOW*>(buffer.data());
    symbol.SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol.MaxNameLen = static_cast<ULONG>(maxSymbolNameLength);
    auto displacement = DWORD64 {0};

    if (!::SymFromAddrW(process, address, &displacement, &symbol))
        return {};

    auto name = Strings::narrow(std::wstring_view {symbol.Name, symbol.NameLen});
    return name + " + " + std::to_string(displacement);
}

bool hasPdbSignature(const IMAGEHLP_MODULEW64& module)
{
    return module.PdbSig70 != GUID {};
}

// The PDB's CodeView GUID and age: lower-case 8-4-4-4-12
// followed by -<age hex>.
std::string formatDebugId(const GUID& guid, DWORD age)
{
    constexpr auto debugIdLength = std::size_t {64};
    auto text = std::array<char, debugIdLength> {};
    std::snprintf(text.data(),
                  text.size(),
                  "%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x-%lx",
                  guid.Data1,
                  guid.Data2,
                  guid.Data3,
                  guid.Data4[0],
                  guid.Data4[1],
                  guid.Data4[2],
                  guid.Data4[3],
                  guid.Data4[4],
                  guid.Data4[5],
                  guid.Data4[6],
                  guid.Data4[7],
                  age);

    return text.data();
}

StackFrame
    makeReportFrame(HANDLE process, std::uint64_t address, bool isReturnAddress)
{
    auto frame = StackFrame {};

    // Symbols first: with deferred loads the PDB, and so its signature, is
    // only read once something asks for a symbol in the module.
    auto lookupBackoff = std::uint64_t {isReturnAddress ? 1u : 0u};
    frame.symbol = findSymbolName(process, address - lookupBackoff);

    auto module = IMAGEHLP_MODULEW64 {};
    module.SizeOfStruct = sizeof(module);

    if (::SymGetModuleInfoW64(process, address, &module))
    {
        frame.modulePath = Strings::narrow(module.LoadedImageName);
        frame.moduleBase = module.BaseOfImage;

        if (hasPdbSignature(module))
            frame.moduleUuid = formatDebugId(module.PdbSig70, module.PdbAge);
    }

    frame.offset = address - frame.moduleBase;
    return frame;
}

Vector<StackFrame> walkCrashedThread(HANDLE process, const SharedCrashInfo& crash)
{
    auto frames = Vector<StackFrame> {};
    auto context = readCrashContext(process, crash);
    auto thread =
        ::OpenThread(THREAD_ALL_ACCESS, FALSE, static_cast<DWORD>(crash.threadId));

    if (!context || thread == nullptr)
        return frames;

    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    ::SymInitializeW(process, nullptr, TRUE);

    auto stackFrame = makeFirstStackFrame(*context);

    while (frames.size() < static_cast<int>(maxCrashFrames)
           && ::StackWalk64(stackWalkMachine,
                            process,
                            thread,
                            &stackFrame,
                            &*context,
                            nullptr,
                            ::SymFunctionTableAccess64,
                            ::SymGetModuleBase64,
                            nullptr)
           && stackFrame.AddrPC.Offset != 0)
    {
        auto isReturnAddress = !frames.empty();
        frames.add(
            makeReportFrame(process, stackFrame.AddrPC.Offset, isReturnAddress));
    }

    ::SymCleanup(process);
    ::CloseHandle(thread);
    return frames;
}

int handleCrash(const ReportDatabase& database, const AppConnection& connection)
{
    const auto& state = connection.state->get();
    auto report = beginHandlerReport(database, state, CaptureSource::handler);
    auto minidumpPath = report.folder / minidumpFileName;

    if (writeMinidump(connection.appProcess, state, minidumpPath))
        report.minidumpPath = minidumpPath;

    report.crashingThread.frames =
        walkCrashedThread(connection.appProcess, state.crash);
    finishHandlerReport(database, report, state);

    auto exitCode = static_cast<UINT>(state.crash.code);
    ::TerminateProcess(connection.appProcess, exitCode);
    ::WaitForSingleObject(connection.appProcess, appTerminationTimeoutMs);

    return deliverReportInHandler(report);
}

bool isCrashExitCode(DWORD exitCode)
{
    return (exitCode & errorSeverityMask) == errorSeverityMask;
}

std::string describeExitCode(DWORD exitCode)
{
    if (auto name = findExceptionName(exitCode))
        return std::string {*name};

    return formatHexAddress(exitCode);
}

// The app ended without telling us: a clean exit, or a crash nothing in it
// could catch (__fastfail, heap corruption), which only its exit code shows.
int handleAppExit(const ReportDatabase& database, const AppConnection& connection)
{
    auto exitCode = DWORD {0};
    ::GetExitCodeProcess(connection.appProcess, &exitCode);

    if (!isCrashExitCode(exitCode))
        return EXIT_SUCCESS;

    const auto& state = connection.state->get();
    auto report = beginHandlerReport(database, state, CaptureSource::exitCode);
    report.reason = describeExitCode(exitCode);
    finishHandlerReport(database, report, state);

    return deliverReportInHandler(report);
}

} // namespace
int runHandlerProcess(const HandlerArguments& arguments)
{
    auto connection = AppConnection {};

    if (!connectToApp(connection, arguments.channel))
        return handlerFailedExitCode;

    signalHandlerReady(arguments.channel);
    auto database = ReportDatabase {arguments.databaseDirectory};

    if (waitForAppEvent(connection) == AppEvent::crash)
    {
        if (connection.state->get().stopRequested.load(std::memory_order_acquire))
            return EXIT_SUCCESS;

        return handleCrash(database, connection);
    }

    return handleAppExit(database, connection);
}

} // namespace eacp::CrashReporting::Detail
