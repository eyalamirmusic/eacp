#include "CrashChannel-macOS.h"
#include "CrashClient-macOS.h"
#include "CrashReportingPlatform.h"
#include "MachImages-macOS.h"
#include "UncaughtExceptions-macOS.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cxxabi.h>
#include <dlfcn.h>
#include <exception>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mutex>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto alternateSignalStackSize = std::size_t {256 * 1024};
constexpr auto demangleBufferSize = std::size_t {1024};
constexpr auto handlerCaptureTimeoutSeconds = 60;
constexpr auto terminateReason = std::string_view {"std::terminate"};
constexpr auto unknownExceptionType = std::string_view {"unknown exception"};
constexpr auto messageSeparator = std::string_view {": "};

struct CaughtSignal
{
    int number;
    std::string_view name;
};

constexpr auto caughtSignals = std::to_array<CaughtSignal>({
    {SIGSEGV, "SIGSEGV"},
    {SIGBUS, "SIGBUS"},
    {SIGILL, "SIGILL"},
    {SIGFPE, "SIGFPE"},
    {SIGABRT, "SIGABRT"},
    {SIGTRAP, "SIGTRAP"},
    {SIGSYS, "SIGSYS"},
});

using SignalAction = struct sigaction;

// Everything the signal handler touches, laid out by start().
struct MacCrashClient
{
    std::optional<std::reference_wrapper<SharedState>> state;
    pid_t handlerProcessId = 0;
    int sharedStateDescriptor = -1;
    int crashPipeWriteEnd = -1;
    int readyPipeReadEnd = -1;
    char* demangleBuffer = nullptr;
    std::size_t demangleBufferLength = 0;
    std::atomic<bool> isCrashing {false};
    std::mutex imageMutex;
    std::array<SignalAction, caughtSignals.size()> previousSignalActions {};
    std::terminate_handler previousTerminateHandler = nullptr;
};

MacCrashClient& getMacCrashClient()
{
    static auto client = MacCrashClient {};
    return client;
}

std::string_view findCaughtSignalName(int signalNumber)
{
    for (const auto& caught: caughtSignals)
    {
        if (caught.number == signalNumber)
            return caught.name;
    }

    return "SIGNAL";
}

std::uint64_t getInterruptedProgramCounter(const ucontext_t& context)
{
#if defined(__arm64__)
    auto programCounter = std::uint64_t {context.uc_mcontext->__ss.__pc};
#else
    auto programCounter = std::uint64_t {context.uc_mcontext->__ss.__rip};
#endif
    return stripPointerAuthentication(programCounter);
}

bool hasRecordedReason(const SharedCrashInfo& crash)
{
    return crash.reason[0] != '\0';
}

void recordSignal(SharedCrashInfo& crash,
                  int signalNumber,
                  const siginfo_t& info,
                  const ucontext_t& context)
{
    if (!hasRecordedReason(crash))
        copyBoundedText(crash.reason, findCaughtSignalName(signalNumber));

    auto faultAddress = reinterpret_cast<std::uintptr_t>(info.si_addr);
    crash.code = static_cast<std::uint64_t>(signalNumber);
    crash.faultAddress = faultAddress;
    crash.threadId = getCurrentThreadId();
    crash.frames[0] = getInterruptedProgramCounter(context);

    auto returnAddresses = std::array<void*, maxCrashFrames - 1> {};
    auto capacity = static_cast<int>(returnAddresses.size());
    auto count = ::backtrace(returnAddresses.data(), capacity);

    for (auto index = 0; index < count; ++index)
    {
        auto address = reinterpret_cast<std::uintptr_t>(returnAddresses[index]);
        crash.frames[index + 1] = stripPointerAuthentication(address);
    }

    crash.frameCount = static_cast<std::uint32_t>(count + 1);
}

bool notifyHandlerProcess(const MacCrashClient& client, char notification)
{
    while (true)
    {
        auto written = ::write(client.crashPipeWriteEnd, &notification, 1);

        if (written == 1)
            return true;

        if (written < 0 && errno == EINTR)
            continue;

        return false;
    }
}

// The handler ends this process once the report is written; the deadline is
// only for a handler that dies part way.
void waitForHandlerToEndProcess()
{
    for (auto second = 0; second < handlerCaptureTimeoutSeconds; ++second)
        ::sleep(1);
}

void restoreDefaultAndRaise(int signalNumber)
{
    auto action = SignalAction {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    ::sigaction(signalNumber, &action, nullptr);
    ::raise(signalNumber);
}

void handleCrashSignal(int signalNumber, siginfo_t* info, void* context)
{
    auto& client = getMacCrashClient();

    if (client.isCrashing.exchange(true) || !client.state)
    {
        waitForHandlerToEndProcess();
        restoreDefaultAndRaise(signalNumber);
        return;
    }

    auto& state = client.state->get();
    const auto& userContext = *static_cast<const ucontext_t*>(context);
    recordSignal(state.crash, signalNumber, *info, userContext);
    state.tableSequence.fetch_add(2, std::memory_order_release);

    if (notifyHandlerProcess(client, crashNotificationByte))
        waitForHandlerToEndProcess();

    restoreDefaultAndRaise(signalNumber);
}

std::string_view demangleExceptionType(const std::type_info& type)
{
    auto& client = getMacCrashClient();
    auto status = 0;
    auto* demangled = abi::__cxa_demangle(
        type.name(), client.demangleBuffer, &client.demangleBufferLength, &status);

    if (status != 0 || demangled == nullptr)
        return type.name();

    client.demangleBuffer = demangled;
    return demangled;
}

void recordCurrentException()
{
    auto exception = std::current_exception();

    if (!exception)
    {
        recordCrashMessage(terminateReason, {}, {});
        return;
    }

    try
    {
        std::rethrow_exception(exception);
    }
    catch (const std::exception& error)
    {
        recordCrashMessage(
            terminateReason, demangleExceptionType(typeid(error)), error.what());
    }
    catch (...)
    {
        recordCrashMessage(terminateReason, unknownExceptionType, {});
    }
}

[[noreturn]] void handleTerminate()
{
    recordCurrentException();
    std::abort();
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

void appendSharedModule(const mach_header* header, std::string_view path)
{
    auto& client = getMacCrashClient();

    if (!client.state)
        return;

    const auto& header64 = *reinterpret_cast<const mach_header_64*>(header);
    auto commandBytes = sizeof(mach_header_64) + header64.sizeofcmds;
    auto lock = std::lock_guard {client.imageMutex};
    auto& crash = client.state->get().crash;

    if (crash.moduleCount >= maxSharedModules)
        return;

    auto& module = crash.modules[crash.moduleCount];
    module.base = reinterpret_cast<std::uintptr_t>(header);
    module.size = getTextSegmentSize(header64, commandBytes);
    copyBoundedText(module.path, path);
    ++crash.moduleCount;
}

void recordLoadedImage(const mach_header* header, intptr_t)
{
    auto info = Dl_info {};

    if (::dladdr(header, &info) != 0 && info.dli_fname != nullptr)
        appendSharedModule(header, info.dli_fname);
}

// dyld is not one of the images it reports, and the bottom of every stack is
// in it.
void recordDynamicLinker()
{
    if (auto dynamicLinker = findDynamicLinkerImage())
        appendSharedModule(dynamicLinker->header, dynamicLinker->path);
}

void installAlternateSignalStack()
{
    static auto stackMemory = std::array<std::byte, alternateSignalStackSize> {};

    auto stack = stack_t {};
    stack.ss_sp = stackMemory.data();
    stack.ss_size = alternateSignalStackSize;
    stack.ss_flags = 0;
    ::sigaltstack(&stack, nullptr);
}

void installSignalHandlers()
{
    auto action = SignalAction {};
    action.sa_sigaction = &handleCrashSignal;
    action.sa_flags = SA_ONSTACK | SA_SIGINFO;
    sigemptyset(&action.sa_mask);

    auto& previous = getMacCrashClient().previousSignalActions;

    for (auto index = std::size_t {0}; index < caughtSignals.size(); ++index)
        ::sigaction(caughtSignals[index].number, &action, &previous[index]);
}

void restorePreviousSignalHandlers()
{
    const auto& previous = getMacCrashClient().previousSignalActions;

    for (auto index = std::size_t {0}; index < caughtSignals.size(); ++index)
        ::sigaction(caughtSignals[index].number, &previous[index], nullptr);
}

void waitForHandlerExit(pid_t handlerProcessId)
{
    auto status = 0;

    while (::waitpid(handlerProcessId, &status, 0) < 0 && errno == EINTR)
        continue;
}

void primeBacktrace()
{
    auto returnAddresses = std::array<void*, 1> {};
    ::backtrace(returnAddresses.data(), static_cast<int>(returnAddresses.size()));
}

void prepareDemangleBuffer()
{
    auto& client = getMacCrashClient();
    client.demangleBufferLength = demangleBufferSize;
    client.demangleBuffer = static_cast<char*>(::malloc(demangleBufferSize));
}

int createUnlinkedSharedFile()
{
    auto pattern = (FilePath::tempDirectory() / "eacp-crash-XXXXXX").str();
    auto descriptor = ::mkstemp(pattern.data());

    if (descriptor >= 0)
        ::unlink(pattern.c_str());

    return descriptor;
}

void setCloseOnExec(int descriptor)
{
    ::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
}

struct PipeEnds
{
    int readEnd = -1;
    int writeEnd = -1;
};

std::optional<PipeEnds> createPipe()
{
    auto ends = std::array<int, 2> {};

    if (::pipe(ends.data()) != 0)
        return std::nullopt;

    setCloseOnExec(ends[0]);
    setCloseOnExec(ends[1]);
    return PipeEnds {ends[0], ends[1]};
}

// A copy numbered past the handler's fixed slots, so moving one end into its
// slot can never overwrite another end still waiting to move, and dup2 onto
// itself never leaves close-on-exec set.
struct DescriptorAboveHandlerSlots
{
    explicit DescriptorAboveHandlerSlots(int original)
        : descriptor(::fcntl(original, F_DUPFD_CLOEXEC, firstFreeDescriptor))
    {
    }

    ~DescriptorAboveHandlerSlots() { ::close(descriptor); }

    int descriptor;
};

Vector<char*> makeArgumentPointers(Vector<std::string>& arguments)
{
    auto pointers = Vector<char*> {};

    for (auto& argument: arguments)
        pointers.add(argument.data());

    pointers.add(nullptr);
    return pointers;
}

std::optional<pid_t> spawnHandler(const HandlerLaunch& launch,
                                  int sharedDescriptor,
                                  const PipeEnds& crashPipe,
                                  const PipeEnds& readyPipe)
{
    auto arguments = makeHandlerCommandLine(launch, handlerDescriptorChannel);
    auto argumentPointers = makeArgumentPointers(arguments);

    auto actions = posix_spawn_file_actions_t {};
    ::posix_spawn_file_actions_init(&actions);
    auto movedShared = DescriptorAboveHandlerSlots {sharedDescriptor};
    auto movedCrashPipe = DescriptorAboveHandlerSlots {crashPipe.readEnd};
    auto movedReadyPipe = DescriptorAboveHandlerSlots {readyPipe.writeEnd};
    ::posix_spawn_file_actions_adddup2(
        &actions, movedShared.descriptor, handlerSharedStateDescriptor);
    ::posix_spawn_file_actions_adddup2(
        &actions, movedCrashPipe.descriptor, handlerCrashPipeDescriptor);
    ::posix_spawn_file_actions_adddup2(
        &actions, movedReadyPipe.descriptor, handlerReadyPipeDescriptor);

    for (auto standardDescriptor: {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
        ::posix_spawn_file_actions_addinherit_np(&actions, standardDescriptor);

    auto attributes = posix_spawnattr_t {};
    ::posix_spawnattr_init(&attributes);
    ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);

    auto processId = pid_t {};
    auto result = ::posix_spawn(&processId,
                                launch.executable.str().c_str(),
                                &actions,
                                &attributes,
                                argumentPointers.data(),
                                environ);

    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
    if (result != 0)
        return std::nullopt;

    return processId;
}

bool waitForHandlerReady(int readyReadEnd, std::chrono::milliseconds timeout)
{
    auto descriptor = pollfd {readyReadEnd, POLLIN, 0};
    auto timeoutMilliseconds = static_cast<int>(timeout.count());

    if (::poll(&descriptor, 1, timeoutMilliseconds) <= 0)
        return false;

    auto byte = char {};
    return ::read(readyReadEnd, &byte, 1) == 1 && byte == handlerReadyByte;
}

} // namespace

void recordCrashMessage(std::string_view reason,
                        std::string_view name,
                        std::string_view detail)
{
    auto& client = getMacCrashClient();

    if (!client.state)
        return;

    auto& crash = client.state->get().crash;

    if (hasRecordedReason(crash))
        return;

    copyBoundedText(crash.reason, reason);

    auto length = std::size_t {0};
    appendBoundedText(crash.message, length, name);

    if (!name.empty() && !detail.empty())
        appendBoundedText(crash.message, length, messageSeparator);

    appendBoundedText(crash.message, length, detail);
}

bool notifyHandlerOfHang()
{
    auto& client = getMacCrashClient();

    if (client.crashPipeWriteEnd < 0 || client.isCrashing.load())
        return false;

    return notifyHandlerProcess(client, hangNotificationByte);
}

void installCrashHandlers(SharedState& state)
{
    auto& client = getMacCrashClient();
    client.state = state;

    primeBacktrace();
    prepareDemangleBuffer();
    recordDynamicLinker();
    _dyld_register_func_for_add_image(&recordLoadedImage);
    installAlternateSignalStack();
    installSignalHandlers();
    client.previousTerminateHandler = std::set_terminate(&handleTerminate);
    installUncaughtExceptionRecorder();
}

void uninstallCrashHandlers()
{
    auto& client = getMacCrashClient();
    restorePreviousSignalHandlers();
    std::set_terminate(client.previousTerminateHandler);
    uninstallUncaughtExceptionRecorder();
}

// EOF on the crash pipe with no byte before it is the handler's clean exit.
void stopHandlerProcess(SharedState& state)
{
    auto& client = getMacCrashClient();
    state.stopRequested.store(1, std::memory_order_release);

    if (client.crashPipeWriteEnd >= 0)
    {
        ::close(client.crashPipeWriteEnd);
        client.crashPipeWriteEnd = -1;
    }

    if (client.handlerProcessId != 0)
    {
        waitForHandlerExit(client.handlerProcessId);
        client.handlerProcessId = 0;
    }
}

SharedState& createSharedState(const HandlerLaunch&)
{
    auto& client = getMacCrashClient();
    auto descriptor = createUnlinkedSharedFile();
    auto size = static_cast<off_t>(sizeof(SharedState));

    if (descriptor >= 0 && ::ftruncate(descriptor, size) == 0)
    {
        auto* mapping = ::mmap(nullptr,
                               sizeof(SharedState),
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED,
                               descriptor,
                               0);

        if (mapping != MAP_FAILED)
        {
            setCloseOnExec(descriptor);
            client.sharedStateDescriptor = descriptor;
            auto& state = *static_cast<SharedState*>(mapping);
            initialiseSharedState(state, getCurrentProcessId());
            return state;
        }
    }

    static auto privateState = SharedState {};
    initialiseSharedState(privateState, getCurrentProcessId());
    return privateState;
}

bool launchHandlerProcess(const HandlerLaunch& launch, SharedState&)
{
    auto& client = getMacCrashClient();
    auto crashPipe = createPipe();
    auto readyPipe = createPipe();

    if (client.sharedStateDescriptor < 0 || !crashPipe || !readyPipe)
        return false;

    auto spawned =
        spawnHandler(launch, client.sharedStateDescriptor, *crashPipe, *readyPipe);
    ::close(crashPipe->readEnd);
    ::close(readyPipe->writeEnd);

    auto ready = spawned.has_value()
                 && waitForHandlerReady(readyPipe->readEnd, launch.startTimeout);
    ::close(readyPipe->readEnd);

    if (!ready)
    {
        ::close(crashPipe->writeEnd);
        return false;
    }

    ::fcntl(crashPipe->writeEnd, F_SETNOSIGPIPE, 1);
    client.crashPipeWriteEnd = crashPipe->writeEnd;
    client.handlerProcessId = *spawned;
    return true;
}

bool isHandlerProcessRunning()
{
    auto& client = getMacCrashClient();

    if (client.handlerProcessId == 0)
        return false;

    auto status = 0;
    return ::waitpid(client.handlerProcessId, &status, WNOHANG) == 0;
}

std::string getOperatingSystemVersion()
{
    auto version = std::array<char, maxReasonLength> {};
    auto length = version.size();

    if (::sysctlbyname("kern.osproductversion", version.data(), &length, nullptr, 0)
        != 0)
        return {};

    return std::string {readBoundedText(version)};
}

std::uint64_t getCurrentProcessId()
{
    return static_cast<std::uint64_t>(::getpid());
}

std::uint64_t getCurrentThreadId()
{
    auto threadId = std::uint64_t {0};
    ::pthread_threadid_np(nullptr, &threadId);
    return threadId;
}

} // namespace eacp::CrashReporting::Detail
