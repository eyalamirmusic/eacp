#include "CrashReportingPlatform.h"
#include "SymbolNames-Windows.h"

#include <array>
#include <mutex>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto maxEventFrames = ULONG {62};

// DbgHelp is single-threaded, and initialised in the app only for events.
std::mutex& getSymbolMutex()
{
    static auto mutex = std::mutex {};
    return mutex;
}

HANDLE getSymbolProcess()
{
    static auto* process = []
    {
        auto* self = ::GetCurrentProcess();
        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        ::SymInitializeW(self, nullptr, TRUE);
        return self;
    }();

    return process;
}
} // namespace

Vector<StackFrame> captureCurrentStack(int framesToSkip)
{
    constexpr auto isReturnAddress = true;
    auto addresses = std::array<void*, maxEventFrames> {};
    auto skip = static_cast<ULONG>(framesToSkip + 1);
    auto count =
        ::RtlCaptureStackBackTrace(skip, maxEventFrames, addresses.data(), nullptr);

    auto lock = std::lock_guard {getSymbolMutex()};
    auto* process = getSymbolProcess();
    auto frames = Vector<StackFrame> {};

    for (auto index = USHORT {0}; index < count; ++index)
    {
        auto address = reinterpret_cast<std::uintptr_t>(addresses[index]);
        frames.add(makeReportFrame(process, address, isReturnAddress));
    }

    return frames;
}

std::string getTypeName(const std::type_info& type)
{
    return std::string {getExceptionTypeName(type)};
}
} // namespace eacp::CrashReporting::Detail
