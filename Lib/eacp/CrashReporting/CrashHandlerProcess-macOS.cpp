#include "CrashChannel-macOS.h"
#include "CrashReportingPlatform.h"
#include "LoadedLibrarySymbolReader-macOS.h"
#include "MachImages-macOS.h"
#include "Minidump-macOS.h"
#include "TaskCapture-macOS.h"
#include "TaskPortHandshake-macOS.h"

#include <eacp/Core/Threads/Thread.h>
#include <eacp/Core/Utils/Format.h>

#include <cerrno>
#include <csignal>
#include <cxxabi.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <memory>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto signalTrampolineSymbol = std::string_view {"_sigtramp"};

enum class CrashPipeEvent
{
    crash,
    hang,
    appExited
};

CrashPipeEvent waitForCrashPipeEvent()
{
    auto byte = char {};

    while (true)
    {
        auto count = ::read(handlerCrashPipeDescriptor, &byte, 1);

        if (count < 0 && errno == EINTR)
            continue;

        if (count == 1 && byte == crashNotificationByte)
            return CrashPipeEvent::crash;

        if (count == 1 && byte == hangNotificationByte)
            return CrashPipeEvent::hang;

        return CrashPipeEvent::appExited;
    }
}

std::optional<std::reference_wrapper<SharedState>> mapInheritedSharedState()
{
    auto* mapping = ::mmap(nullptr,
                           sizeof(SharedState),
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           handlerSharedStateDescriptor,
                           0);

    if (mapping == MAP_FAILED)
        return std::nullopt;

    auto& state = *static_cast<SharedState*>(mapping);

    if (!isSharedStateValid(state))
        return std::nullopt;

    return state;
}

void signalHandlerReady()
{
    ::write(handlerReadyPipeDescriptor, &handlerReadyByte, 1);
    ::close(handlerReadyPipeDescriptor);
}
std::map<std::string, std::uint64_t> getHandlerImageBases()
{
    auto bases = std::map<std::string, std::uint64_t> {};

    for (auto index = 0u; index < ::_dyld_image_count(); ++index)
    {
        const auto* header = ::_dyld_get_image_header(index);
        const auto* name = ::_dyld_get_image_name(index);

        if (header != nullptr && name != nullptr)
            bases[name] = reinterpret_cast<std::uintptr_t>(header);
    }

    if (auto dynamicLinker = findDynamicLinkerImage())
        bases[dynamicLinker->path] =
            reinterpret_cast<std::uintptr_t>(dynamicLinker->header);

    return bases;
}

std::string demangleSymbol(const char* symbol)
{
    auto status = 0;
    auto demangled = std::unique_ptr<char, decltype(&::free)> {
        abi::__cxa_demangle(symbol, nullptr, nullptr, &status), &::free};

    if (status != 0 || !demangled)
        return symbol;

    return demangled.get();
}

// The libraries the app loaded and the handler did not, read from their files.
using LoadedLibrarySymbolReaders =
    std::map<std::string, std::unique_ptr<LoadedLibrarySymbolReader>>;

const LoadedLibrarySymbolReader&
    getOrOpenLibrarySymbolReader(LoadedLibrarySymbolReaders& files,
                                 const std::string& path)
{
    auto& file = files[path];

    if (!file)
        file = std::make_unique<LoadedLibrarySymbolReader>(FilePath {path});

    return *file;
}

std::string resolveFunctionNameFromLibraryFile(const LoadedLibrarySymbolReader& file,
                                               std::uint64_t offset,
                                               std::uint64_t lookupBackoff)
{
    auto symbol = file.findFunctionContainingOffset(offset - lookupBackoff);

    if (!symbol)
        return {};

    return demangleSymbol(symbol->mangledName.c_str()) + " + "
           + std::to_string(symbol->distance + lookupBackoff);
}

std::string resolveSymbol(std::uint64_t handlerAddress, std::uint64_t lookupBackoff)
{
    auto info = Dl_info {};
    auto* address = reinterpret_cast<void*>(handlerAddress - lookupBackoff);

    if (::dladdr(address, &info) == 0 || info.dli_sname == nullptr)
        return {};

    auto symbolStart = reinterpret_cast<std::uintptr_t>(info.dli_saddr);
    auto distance = handlerAddress - symbolStart;
    return demangleSymbol(info.dli_sname) + " + " + std::to_string(distance);
}

// A return address points past its call, at whatever follows, which after a
// call that never returns is the next function; one byte back is the call.
StackFrame makeStackFrame(const SharedCrashInfo& crash,
                          const std::map<std::string, std::uint64_t>& handlerBases,
                          LoadedLibrarySymbolReaders& librarySymbolReaders,
                          std::uint64_t address,
                          bool isReturnAddress)
{
    auto frame = StackFrame {};
    auto index = findSharedModuleIndex(crash, address);

    if (!index)
    {
        frame.offset = address;
        return frame;
    }

    const auto& module = crash.modules[*index];
    frame.modulePath = std::string {readBoundedText(module.path)};
    frame.moduleBase = module.base;
    frame.offset = address - module.base;

    auto lookupBackoff = std::uint64_t {isReturnAddress ? 1u : 0u};

    if (auto base = handlerBases.find(frame.modulePath); base != handlerBases.end())
    {
        frame.symbol = resolveSymbol(base->second + frame.offset, lookupBackoff);
        const auto* header = reinterpret_cast<const mach_header_64*>(base->second);
        frame.moduleUuid = getImageUuid(*header);
        return frame;
    }

    const auto& file =
        getOrOpenLibrarySymbolReader(librarySymbolReaders, frame.modulePath);
    frame.symbol =
        resolveFunctionNameFromLibraryFile(file, frame.offset, lookupBackoff);
    frame.moduleUuid = file.getBuildUuid();
    return frame;
}

bool isSignalTrampoline(const StackFrame& frame)
{
    return frame.symbol.starts_with(signalTrampolineSymbol);
}

// The first frame is the faulting instruction; backtrace's frames up to and
// including the signal trampoline are the reporter's own.
void dropSignalHandlerFrames(Vector<StackFrame>& frames)
{
    auto trampoline = std::find_if(frames.begin(), frames.end(), isSignalTrampoline);

    if (trampoline != frames.end())
        frames.erase(frames.begin() + 1, trampoline + 1);
}

enum class FirstFrame
{
    faultingInstruction,
    returnAddress
};

// The modules are the crash record's, which the app keeps current as images
// load, so they serve a hang's addresses as well.
Vector<StackFrame>
    resolveFrames(const SharedCrashInfo& crash,
                  const std::array<std::uint64_t, maxCrashFrames>& addresses,
                  std::uint32_t frameCount,
                  FirstFrame firstFrame = FirstFrame::faultingInstruction)
{
    auto handlerBases = getHandlerImageBases();
    auto librarySymbolReaders = LoadedLibrarySymbolReaders {};
    auto count = std::min(static_cast<std::size_t>(frameCount), maxCrashFrames);
    auto frames = Vector<StackFrame> {};

    for (auto index = std::size_t {0}; index < count; ++index)
    {
        auto isReturnAddress = index > 0 || firstFrame == FirstFrame::returnAddress;
        frames.add(makeStackFrame(crash,
                                  handlerBases,
                                  librarySymbolReaders,
                                  addresses[index],
                                  isReturnAddress));
    }

    return frames;
}

Vector<StackFrame> resolveCrashFrames(const SharedCrashInfo& crash)
{
    auto frames = resolveFrames(crash, crash.frames, crash.frameCount);
    dropSignalHandlerFrames(frames);
    return frames;
}

void handleHang(const ReportDatabase& database, const SharedState& state)
{
    auto report = beginHangReport(database, state);
    report.crashingThread.frames =
        resolveFrames(state.crash, state.hang.frames, state.hang.frameCount);
    finishHandlerReport(database, report, state);
    deliverReportInHandler(report);
}

int handleCrash(const ReportDatabase& database, const SharedState& state)
{
    auto report = beginHandlerReport(database, state, CaptureSource::handler);
    report.crashingThread.frames = resolveCrashFrames(state.crash);

    if (state.crash.throwFrameCount > 0)
    {
        report.terminateFrames = std::move(report.crashingThread.frames);
        report.crashingThread.frames = resolveFrames(state.crash,
                                                     state.crash.throwFrames,
                                                     state.crash.throwFrameCount,
                                                     FirstFrame::returnAddress);
    }

    finishHandlerReport(database, report, state);

    auto appProcessId = static_cast<pid_t>(state.appProcessId);
    ::kill(appProcessId, SIGKILL);

    return deliverReportInHandler(report);
}

constexpr auto handshakeTimeout = std::chrono::seconds {5};
constexpr auto maxMinidumpStackBytes = std::size_t {1024 * 1024};
constexpr auto minidumpFileName = "minidump.dmp";

void copyImagesIntoCrashInfo(SharedCrashInfo& crash, const Vector<AppImage>& images)
{
    auto count = std::min(static_cast<std::size_t>(images.size()), maxSharedModules);

    for (auto index = std::size_t {0}; index < count; ++index)
    {
        const auto& image = images[static_cast<int>(index)];
        auto& module = crash.modules[index];
        module.base = image.base;
        module.size = image.size;
        copyBoundedText(module.path, image.path);
    }

    crash.moduleCount = static_cast<std::uint32_t>(count);
}

void copyFramesIntoCrashInfo(SharedCrashInfo& crash,
                             const Vector<std::uint64_t>& addresses)
{
    auto count =
        std::min(static_cast<std::size_t>(addresses.size()), maxCrashFrames);

    for (auto index = std::size_t {0}; index < count; ++index)
        crash.frames[index] = addresses[static_cast<int>(index)];

    crash.frameCount = static_cast<std::uint32_t>(count);
}

std::uint64_t findExceptionAddress(const MachException& exception,
                                   const TaskThreadRegisters& registers)
{
    if (exception.type == EXC_BAD_ACCESS)
        return static_cast<std::uint64_t>(exception.codes[1]);

    return registers.programCounter;
}

std::string describeMachException(const MachException& exception)
{
    auto code = static_cast<std::uint64_t>(exception.codes[0]);
    auto subcode = static_cast<std::uint64_t>(exception.codes[1]);

    if (exception.type == EXC_BAD_ACCESS)
        return Strings::format("{} at {:#x}",
                               ::mach_error_string(static_cast<kern_return_t>(code)),
                               subcode);

    return Strings::format("code {:#x}, subcode {:#x}", code, subcode);
}

std::uint32_t getProcessorCount()
{
    auto count = std::uint32_t {0};
    auto length = sizeof(count);
    ::sysctlbyname("hw.ncpu", &count, &length, nullptr, 0);
    return count;
}

std::string getOperatingSystemBuild()
{
    auto build = std::array<char, maxReasonLength> {};
    auto length = build.size();

    if (::sysctlbyname("kern.osversion", build.data(), &length, nullptr, 0) != 0)
        return {};

    return std::string {readBoundedText(build)};
}

Vector<std::byte> readStackBytes(mach_port_t task, const MemoryRange& stack)
{
    auto bytes = Vector<std::byte>(static_cast<int>(stack.end - stack.start));
    auto destination =
        std::span {bytes.data(), static_cast<std::size_t>(bytes.size())};

    if (!readAppMemory(task, stack.start, destination))
        return {};

    return bytes;
}

struct MachCapture
{
    mach_port_t task = MACH_PORT_NULL;
    const MachException& exception;
    const TaskThreadRegisters& registers;
    const Vector<AppImage>& images;
    std::uint64_t threadId = 0;
};

MinidumpThread makeMinidumpThread(mach_port_t task,
                                  std::uint64_t threadId,
                                  const TaskThreadRegisters& registers)
{
    auto thread = MinidumpThread {};
    thread.threadId = threadId;
    thread.registers = registers;
    auto stack =
        findThreadStack(task, registers.stackPointer, maxMinidumpStackBytes);

    if (stack)
    {
        thread.stack = *stack;
        thread.stackBytes = readStackBytes(task, *stack);
    }

    return thread;
}

// The crashing thread from the exception's registers, then every other
// thread as the suspended task holds it.
Vector<MinidumpThread> readMinidumpThreads(const MachCapture& capture)
{
    auto threads = Vector<MinidumpThread> {
        makeMinidumpThread(capture.task, capture.threadId, capture.registers)};
    auto taskThreads = listTaskThreads(capture.task);

    for (auto thread: taskThreads)
    {
        auto threadId = readThreadId(thread);
        auto registers = readTaskThreadRegisters(thread);

        if (threadId != capture.threadId && registers)
            threads.add(makeMinidumpThread(capture.task, threadId, *registers));
    }

    releaseTaskThreads(taskThreads);
    return threads;
}

bool writeMachMinidump(const FilePath& path, const MachCapture& capture)
{
    auto contents = MinidumpContents {};
    contents.exceptionType = capture.exception.type;
    contents.exceptionCodes = capture.exception.codes;
    contents.exceptionAddress =
        findExceptionAddress(capture.exception, capture.registers);
    contents.threads = readMinidumpThreads(capture);
    contents.images = capture.images;
    contents.operatingSystemVersion = getOperatingSystemVersion();
    contents.operatingSystemBuild = getOperatingSystemBuild();
    contents.processorCount = getProcessorCount();
    return writeMinidump(path, contents);
}

// EXC_CRASH's first code carries the signal that is ending the process in
// its top byte.
int getCrashSignal(const MachException& exception)
{
    constexpr auto signalShift = 24;
    constexpr auto signalMask = 0xFF;
    auto code = static_cast<std::uint64_t>(exception.codes[0]);
    return static_cast<int>((code >> signalShift) & signalMask);
}

// "SIGABRT" from sys_signame's "abrt".
std::string getSignalName(int signalNumber)
{
    if (signalNumber <= 0 || signalNumber >= NSIG)
        return "SIGNAL";

    return "SIG" + Strings::toUpper(sys_signame[signalNumber]);
}

// A fault is named by its exception. A signal's default action (EXC_CRASH)
// keeps what the app recorded before it, std::terminate or an NSException,
// and is otherwise named by the signal.
void nameMachCrashReport(CrashReport& report, const MachException& exception)
{
    if (exception.type != EXC_CRASH)
    {
        report.reason = getMachExceptionName(exception.type);
        report.message = describeMachException(exception);
        return;
    }

    if (report.reason.empty())
        report.reason = getSignalName(getCrashSignal(exception));
}

// As the signal path does: an uncaught exception's throw site is the
// crashing thread, and where terminate aborted is kept beside it.
void moveThrowSiteToCrashingThread(CrashReport& report, const SharedCrashInfo& crash)
{
    if (crash.throwFrameCount == 0)
        return;

    report.terminateFrames = std::move(report.crashingThread.frames);
    report.crashingThread.frames = resolveFrames(
        crash, crash.throwFrames, crash.throwFrameCount, FirstFrame::returnAddress);
}

// The task port the app handed over; the one in the message when the
// handshake gave none.
mach_port_t chooseAppTask(const HandlerMachPorts& ports,
                          const MachException& exception)
{
    if (ports.appTask != MACH_PORT_NULL)
        return ports.appTask;

    return exception.task;
}

CrashReport captureMachException(const ReportDatabase& database,
                                 const SharedState& state,
                                 const HandlerMachPorts& ports,
                                 const MachException& exception)
{
    auto task = chooseAppTask(ports, exception);

    // The other threads hold still while their stacks are read; the app is
    // killed afterwards, so nothing resumes it.
    ::task_suspend(task);
    auto registers = readThreadState(exception).value_or(TaskThreadRegisters {});
    auto addresses = walkFramePointers(task, registers, maxCrashFrames);
    auto images = readImageList(task);
    auto threadId = readThreadId(exception.thread);

    auto crash = std::make_unique<SharedCrashInfo>(state.crash);
    copyImagesIntoCrashInfo(*crash, images);
    copyFramesIntoCrashInfo(*crash, addresses);

    auto report = beginHandlerReport(database, state, CaptureSource::handlerMach);
    nameMachCrashReport(report, exception);
    report.faultAddress = findExceptionAddress(exception, registers);
    report.crashingThread.threadId = threadId;
    report.crashingThread.frames =
        resolveFrames(*crash, crash->frames, crash->frameCount);

    if (exception.type == EXC_CRASH)
        moveThrowSiteToCrashingThread(report, *crash);

    auto minidumpPath = report.folder / minidumpFileName;
    auto capture = MachCapture {task, exception, registers, images, threadId};

    if (writeMachMinidump(minidumpPath, capture))
        report.minidumpPath = minidumpPath;

    return report;
}

// The faulting thread is still suspended in the kernel, so the app is ended
// before the reply lets it go on into its own signal handler. The reply says
// handled: a failure would send the exception on to the host's port, where
// ReportCrash holds a guard exception's dying app for minutes.
int handleMachException(const ReportDatabase& database,
                        const SharedState& state,
                        const HandlerMachPorts& ports,
                        const MachException& exception)
{
    auto report = captureMachException(database, state, ports, exception);
    finishHandlerReport(database, report, state);

    ::kill(static_cast<pid_t>(state.appProcessId), SIGKILL);
    replyToException(exception, KERN_SUCCESS);
    releaseExceptionPorts(exception);

    return deliverReportInHandler(report);
}

bool isFromApp(const MachException& exception, const SharedState& state)
{
    auto processId = pid_t {0};

    if (::pid_for_task(exception.task, &processId) != KERN_SUCCESS)
        return false;

    return static_cast<std::uint64_t>(processId) == state.appProcessId;
}

// What the pipe's thread and the exception port's thread share. Whichever
// captures takes the mutex for the whole capture, so the other never reports
// the same crash or exits under it.
struct HandlerSession
{
    HandlerSession(const ReportDatabase& databaseToUse,
                   const SharedState& stateToRead,
                   const HandlerMachPorts& portsFromHandshake)
        : database(databaseToUse)
        , state(stateToRead)
        , ports(portsFromHandshake)
    {
    }

    const ReportDatabase& database;
    const SharedState& state;
    HandlerMachPorts ports;
    std::mutex captureMutex;
    std::optional<int> machCaptureExitCode;

    // Set by whichever path reported first: an abort that then traps can
    // reach both, and the second would find the app already gone.
    bool hasReportedCrash = false;
    std::atomic<bool> isStopping {false};
};

// Every message is answered until the handler stops: after a capture the
// kill raises EXC_CRASH on this port too, and the kernel does not finish
// ending the app until that is replied to. An exception from a child the app
// spawned, which inherits the ports, is handed back to the kernel untouched.
void serveExceptions(HandlerSession& session)
{
    while (!session.isStopping.load())
    {
        auto exception = receiveExceptionMessage(session.ports.exceptionPort);

        if (!exception)
            continue;

        auto lock = std::lock_guard {session.captureMutex};

        if (session.hasReportedCrash)
        {
            replyToException(*exception, KERN_SUCCESS);
            releaseExceptionPorts(*exception);
            continue;
        }

        if (!isFromApp(*exception, session.state))
        {
            replyToException(*exception, KERN_FAILURE);
            releaseExceptionPorts(*exception);
            continue;
        }

        session.hasReportedCrash = true;
        session.machCaptureExitCode = handleMachException(
            session.database, session.state, session.ports, *exception);
    }
}

void stopServingExceptions(HandlerSession& session)
{
    session.isStopping.store(true);

    if (session.ports.exceptionPort != MACH_PORT_NULL)
        ::mach_port_mod_refs(::mach_task_self(),
                             session.ports.exceptionPort,
                             MACH_PORT_RIGHT_RECEIVE,
                             -1);
}

int waitForAppEvents(HandlerSession& session)
{
    while (true)
    {
        auto event = waitForCrashPipeEvent();
        auto lock = std::lock_guard {session.captureMutex};

        if (session.machCaptureExitCode)
            return *session.machCaptureExitCode;

        if (event == CrashPipeEvent::appExited)
            return EXIT_SUCCESS;

        if (event == CrashPipeEvent::crash)
        {
            session.hasReportedCrash = true;
            return handleCrash(session.database, session.state);
        }

        handleHang(session.database, session.state);
    }
}
} // namespace
int runHandlerProcess(const HandlerArguments& arguments)
{
    auto isMachChannel = arguments.channel == machHandshakeHandlerChannel;

    if (!isMachChannel && arguments.channel != signalOnlyHandlerChannel)
        return EXIT_FAILURE;

    auto state = mapInheritedSharedState();

    if (!state)
        return EXIT_FAILURE;

    auto pendingRequest = std::optional<PendingTaskPortRequest> {};

    if (isMachChannel)
        pendingRequest = sendAppTaskPortRequest();

    signalHandlerReady();
    auto ports = std::optional<HandlerMachPorts> {};

    if (pendingRequest)
        ports = receiveAppTaskPort(*pendingRequest, handshakeTimeout);

    auto database = ReportDatabase {arguments.databaseDirectory};
    auto session =
        HandlerSession {database, *state, ports.value_or(HandlerMachPorts {})};
    auto exceptionThread = Threads::Thread {};

    if (ports)
        exceptionThread.start([&session] { serveExceptions(session); });

    auto exitCode = waitForAppEvents(session);
    stopServingExceptions(session);
    exceptionThread.join();
    return exitCode;
}

} // namespace eacp::CrashReporting::Detail
