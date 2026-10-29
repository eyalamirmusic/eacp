#include "ThrowSites-macOS.h"
#include "MachImages-macOS.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <string_view>
#include <typeinfo>

namespace eacp::CrashReporting::Detail
{
namespace
{
using ThrowFunction = void (*)(void*, std::type_info*, void (*)(void*));

// Room for the recorder's own frames, which backtrace reports first.
constexpr auto recorderFrameAllowance = 4;
constexpr auto throwFunctionName = std::string_view {"__cxa_throw"};

struct ThrowSite
{
    void* thrownObject;
    int frameCount;
    std::array<void*, maxCrashFrames + recorderFrameAllowance> frames;
};

thread_local ThrowSite lastThrowSite {};
std::atomic<bool> isRecording {false};

ThrowFunction findRealThrowFunction()
{
    static auto* real =
        reinterpret_cast<ThrowFunction>(::dlsym(RTLD_NEXT, "__cxa_throw"));
    return real;
}

void recordThrowSite(void* thrownObject)
{
    auto& site = lastThrowSite;
    site.thrownObject = thrownObject;
    site.frameCount =
        ::backtrace(site.frames.data(), static_cast<int>(site.frames.size()));
}

bool isInsideThrowFunction(void* returnAddress)
{
    auto info = Dl_info {};
    auto* callSite = static_cast<char*>(returnAddress) - 1;

    return ::dladdr(callSite, &info) != 0 && info.dli_sname != nullptr
           && std::string_view {info.dli_sname} == throwFunctionName;
}

// The frames up to and including __cxa_throw are the recorder's own: one when
// recordThrowSite is inlined into it, more in a build that inlines nothing.
int countRecorderFrames(const ThrowSite& site)
{
    for (auto index = 0; index < site.frameCount; ++index)
    {
        if (isInsideThrowFunction(site.frames[index]))
            return index + 1;
    }

    return 1;
}

bool isCurrentException(void* thrownObject)
{
    auto* current = abi::__cxa_current_primary_exception();

    if (current == nullptr)
        return false;

    abi::__cxa_decrement_exception_refcount(current);
    return current == thrownObject;
}
} // namespace

void startRecordingThrowSites()
{
    findRealThrowFunction();
    isRecording = true;
}

void stopRecordingThrowSites()
{
    isRecording = false;
}

bool copyThrowSiteOfCurrentException(SharedCrashInfo& crash)
{
    const auto& site = lastThrowSite;

    if (!isCurrentException(site.thrownObject))
        return false;

    auto recorderFrames = countRecorderFrames(site);
    auto count =
        std::min(site.frameCount - recorderFrames, static_cast<int>(maxCrashFrames));

    if (count <= 0)
        return false;

    for (auto index = 0; index < count; ++index)
    {
        auto address =
            reinterpret_cast<std::uintptr_t>(site.frames[index + recorderFrames]);
        crash.throwFrames[index] = stripPointerAuthentication(address);
    }

    crash.throwFrameCount = static_cast<std::uint32_t>(count);
    return true;
}
} // namespace eacp::CrashReporting::Detail

// Overrides libc++abi's __cxa_throw for throws compiled into the same
// executable or library as eacp-crash-reporting; other libraries still use
// libc++abi's.
extern "C" void
    __cxa_throw(void* thrownObject, std::type_info* type, void (*destructor)(void*))
{
    using namespace eacp::CrashReporting::Detail;

    if (isRecording.load(std::memory_order_relaxed))
        recordThrowSite(thrownObject);

    findRealThrowFunction()(thrownObject, type, destructor);
    __builtin_unreachable();
}
