#include "FaultDefinitions.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Threads/Thread.h>
#include <eacp/Core/Utils/Singleton.h>
#include <eacp/CrashReporting/CrashReportingPlatform.h>

#include <atomic>
#include <mutex>

namespace eacp::CrashReporting::Faults
{
namespace
{
struct ThreadChoiceName
{
    ThreadChoice choice;
    std::string_view name;
};

constexpr auto threadChoiceNames = Array {
    ThreadChoiceName {ThreadChoice::main, "main"},
    ThreadChoiceName {ThreadChoice::background, "background"},
};

struct BackgroundThreads
{
    std::mutex mutex;
    OwnedVector<Threads::Thread> threads;
};

std::atomic<int> returnedBackgroundFaults {0};

std::optional<std::reference_wrapper<const Fault>>
    findAvailableFault(std::string_view name)
{
    auto fault = findFault(name);

    if (!fault || !fault->get().isAvailable)
        return std::nullopt;

    return fault;
}
} // namespace

// Outside the anonymous namespace so its name reaches the report's symbols,
// and counting afterwards so the call is not a tail call that drops it.
EACP_FAULT_NO_INLINE void runFaultOnBackgroundThread(const Fault& fault)
{
    fault.trigger();
    returnedBackgroundFaults.fetch_add(1);
}

void startBackgroundThread(const Callback& body)
{
    auto& registry = Singleton::getImmortal<BackgroundThreads>();
    auto lock = std::lock_guard {registry.mutex};
    registry.threads.createNew(body);
}

std::string_view getThreadChoiceName(ThreadChoice choice)
{
    for (const auto& entry: threadChoiceNames)
        if (entry.choice == choice)
            return entry.name;

    return {};
}

std::optional<ThreadChoice> findThreadChoice(std::string_view name)
{
    for (const auto& entry: threadChoiceNames)
        if (entry.name == name)
            return entry.choice;

    return std::nullopt;
}

const Vector<Fault>& listFaults()
{
    static const auto faults = makeFaults();
    return faults;
}

std::optional<std::reference_wrapper<const Fault>> findFault(std::string_view name)
{
    for (const auto& fault: listFaults())
    {
        if (fault.name == name)
            return fault;
    }

    return std::nullopt;
}

bool triggerFault(std::string_view name, ThreadChoice thread)
{
    auto fault = findAvailableFault(name);

    if (!fault)
        return false;

    if (thread == ThreadChoice::main)
    {
        fault->get().trigger();
        return true;
    }

    const auto& chosen = fault->get();
    startBackgroundThread([&chosen] { runFaultOnBackgroundThread(chosen); });
    return true;
}

bool scheduleFault(std::string_view name, ThreadChoice thread, Time::MS delay)
{
    auto fault = findAvailableFault(name);

    if (!fault)
        return false;

    auto faultName = fault->get().name;
    Threads::callAfter(delay,
                       [faultName, thread] { triggerFault(faultName, thread); });
    return true;
}

void killHandlerProcess()
{
    Detail::killHandlerProcess();
}
} // namespace eacp::CrashReporting::Faults
