#include "CrashReportingPlatform.h"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <pthread/introspection.h>
#include <sys/mman.h>
#include <unistd.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
// Room for backtrace() and the report's bookkeeping in the signal handler.
constexpr auto alternateStackBytes = std::size_t {256 * 1024};

std::atomic<bool> shouldInstallOnNewThreads {false};
std::optional<pthread_introspection_hook_t> previousIntrospectionHook;

std::size_t getGuardBytes()
{
    return static_cast<std::size_t>(::getpagesize());
}

bool hasAlternateStack()
{
    auto current = stack_t {};

    if (::sigaltstack(nullptr, &current) != 0)
        return false;

    return (current.ss_flags & SS_DISABLE) == 0;
}

// The mapping of the stack the thread is using, which leaves with the thread.
void removeAlternateStackAtThreadExit(void* mapping)
{
    auto disabled = stack_t {};
    disabled.ss_flags = SS_DISABLE;
    ::sigaltstack(&disabled, nullptr);
    ::munmap(mapping, getGuardBytes() + alternateStackBytes);
}

pthread_key_t getAlternateStackKey()
{
    static const auto key = []
    {
        auto created = pthread_key_t {};
        ::pthread_key_create(&created, &removeAlternateStackAtThreadExit);
        return created;
    }();

    return key;
}

void installOnStartingThread(unsigned int event,
                             pthread_t thread,
                             void* address,
                             std::size_t size)
{
    auto isStartingHere =
        event == PTHREAD_INTROSPECTION_THREAD_START && thread == ::pthread_self();

    if (isStartingHere && shouldInstallOnNewThreads.load(std::memory_order_relaxed))
        installStackOverflowStackOnCurrentThread();

    if (previousIntrospectionHook)
        (*previousIntrospectionHook)(event, thread, address, size);
}

void installIntrospectionHookOnce()
{
    static auto once = std::once_flag {};
    std::call_once(once,
                   []
                   {
                       auto previous = ::pthread_introspection_hook_install(
                           &installOnStartingThread);

                       if (previous != nullptr)
                           previousIntrospectionHook = previous;
                   });
}
} // namespace

// Reserved, not touched, so only the pages a crash writes become resident.
bool installStackOverflowStackOnCurrentThread()
{
    if (hasAlternateStack())
        return false;

    auto guardBytes = getGuardBytes();
    auto mappedBytes = guardBytes + alternateStackBytes;
    auto* mapping = ::mmap(
        nullptr, mappedBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    if (mapping == MAP_FAILED)
        return false;

    ::mprotect(mapping, guardBytes, PROT_NONE);

    auto stack = stack_t {};
    stack.ss_sp = static_cast<std::byte*>(mapping) + guardBytes;
    stack.ss_size = alternateStackBytes;

    if (::sigaltstack(&stack, nullptr) != 0)
    {
        ::munmap(mapping, mappedBytes);
        return false;
    }

    ::pthread_setspecific(getAlternateStackKey(), mapping);
    return true;
}

void setStackOverflowStackOnNewThreads(bool shouldInstall)
{
    if (shouldInstall)
        installIntrospectionHookOnce();

    shouldInstallOnNewThreads.store(shouldInstall);
}
} // namespace eacp::CrashReporting::Detail
