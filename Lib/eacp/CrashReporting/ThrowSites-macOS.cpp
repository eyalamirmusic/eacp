#include "ThrowSites-macOS.h"
#include "MachImages-macOS.h"

#include <array>
#include <atomic>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <typeinfo>

namespace eacp::CrashReporting::Detail
{
namespace
{
using ThrowFunction = void (*)(void*, std::type_info*, void (*)(void*));

// backtrace's first address is inside __cxa_throw itself.
constexpr auto throwFunctionFrames = 1;

struct ThrowSite
{
    void* thrownObject;
    int frameCount;
    std::array<void*, maxCrashFrames + throwFunctionFrames> frames;
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

    if (site.frameCount <= throwFunctionFrames
        || !isCurrentException(site.thrownObject))
        return false;

    auto count = site.frameCount - throwFunctionFrames;

    for (auto index = 0; index < count; ++index)
    {
        auto address = reinterpret_cast<std::uintptr_t>(
            site.frames[index + throwFunctionFrames]);
        crash.throwFrames[index] = stripPointerAuthentication(address);
    }

    crash.throwFrameCount = static_cast<std::uint32_t>(count);
    return true;
}
} // namespace eacp::CrashReporting::Detail

// Defined in this image, so every throw compiled into it binds here rather
// than to libc++abi.
extern "C" void
    __cxa_throw(void* thrownObject, std::type_info* type, void (*destructor)(void*))
{
    using namespace eacp::CrashReporting::Detail;

    if (isRecording.load(std::memory_order_relaxed))
        recordThrowSite(thrownObject);

    findRealThrowFunction()(thrownObject, type, destructor);
    __builtin_unreachable();
}
