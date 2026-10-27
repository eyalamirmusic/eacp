#include "CrashChannel-macOS.h"
#include "CrashReportingPlatform.h"
#include "MachImages-macOS.h"

#include <cerrno>
#include <csignal>
#include <cxxabi.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <map>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto signalTrampolineSymbol = std::string_view {"_sigtramp"};

enum class CrashPipeEvent
{
    crash,
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

    if (auto base = handlerBases.find(frame.modulePath); base != handlerBases.end())
    {
        auto lookupBackoff = std::uint64_t {isReturnAddress ? 1u : 0u};
        frame.symbol = resolveSymbol(base->second + frame.offset, lookupBackoff);
        const auto* header = reinterpret_cast<const mach_header_64*>(base->second);
        frame.moduleUuid = getImageUuid(*header);
    }

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

Vector<StackFrame> resolveCrashFrames(const SharedCrashInfo& crash)
{
    auto handlerBases = getHandlerImageBases();
    auto count =
        std::min(static_cast<std::size_t>(crash.frameCount), maxCrashFrames);
    auto frames = Vector<StackFrame> {};

    for (auto index = std::size_t {0}; index < count; ++index)
    {
        auto isReturnAddress = index > 0;
        frames.add(makeStackFrame(
            crash, handlerBases, crash.frames[index], isReturnAddress));
    }

    dropSignalHandlerFrames(frames);
    return frames;
}

int handleCrash(const ReportDatabase& database, const SharedState& state)
{
    auto report = beginHandlerReport(database, state, CaptureSource::handler);
    report.crashingThread.frames = resolveCrashFrames(state.crash);
    finishHandlerReport(database, report, state);

    auto appProcessId = static_cast<pid_t>(state.appProcessId);
    ::kill(appProcessId, SIGKILL);

    return deliverReportInHandler(report);
}

} // namespace
int runHandlerProcess(const HandlerArguments& arguments)
{
    if (arguments.channel != handlerDescriptorChannel)
        return EXIT_FAILURE;

    auto state = mapInheritedSharedState();

    if (!state)
        return EXIT_FAILURE;

    signalHandlerReady();

    if (waitForCrashPipeEvent() == CrashPipeEvent::appExited)
        return EXIT_SUCCESS;

    return handleCrash(ReportDatabase {arguments.databaseDirectory}, *state);
}

} // namespace eacp::CrashReporting::Detail
