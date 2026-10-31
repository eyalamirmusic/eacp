#include "CrashReportingPlatform.h"
#include "MachImages-macOS.h"
#include "SymbolNames-macOS.h"

#include <eacp/Core/Utils/Strings.h>

#include <dlfcn.h>
#include <execinfo.h>

#include <array>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto maxEventFrames = 128;

// Every address backtrace() gives is a return address, so one byte back is
// the call itself.
StackFrame makeEventFrame(std::uint64_t address)
{
    auto frame = StackFrame {};
    auto info = Dl_info {};
    auto* call = reinterpret_cast<void*>(address - 1);

    if (::dladdr(call, &info) == 0)
    {
        frame.offset = address;
        return frame;
    }

    frame.moduleBase = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    frame.offset = address - frame.moduleBase;

    if (info.dli_fname != nullptr)
        frame.modulePath = info.dli_fname;

    frame.moduleUuid =
        getImageUuid(*static_cast<const mach_header_64*>(info.dli_fbase));

    if (info.dli_sname != nullptr)
    {
        auto symbolStart = reinterpret_cast<std::uintptr_t>(info.dli_saddr);
        frame.symbol = Strings::concat(
            demangleSymbol(info.dli_sname), " + ", address - symbolStart);
    }

    return frame;
}
} // namespace

Vector<StackFrame> captureCurrentStack(int framesToSkip)
{
    auto addresses = std::array<void*, maxEventFrames> {};
    auto count = ::backtrace(addresses.data(), maxEventFrames);
    auto frames = Vector<StackFrame> {};

    // This function's own frame comes first.
    for (auto index = framesToSkip + 1; index < count; ++index)
    {
        auto address = reinterpret_cast<std::uintptr_t>(addresses[index]);
        frames.add(makeEventFrame(stripPointerAuthentication(address)));
    }

    return frames;
}

std::string getTypeName(const std::type_info& type)
{
    return demangleSymbol(type.name());
}
} // namespace eacp::CrashReporting::Detail
