#pragma once

#include "../Common.h"
#include "ProcessChildModes.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>

// What the requestStop() cases share: waiting for the awaitStopRequest child to
// say it is ready, and for a child to exit, each with a deadline.
namespace ProcessStopping
{
constexpr auto readyDeadline = std::chrono::seconds {30};
constexpr auto exitDeadline = std::chrono::seconds {10};

inline bool waitUntil(const std::function<bool()>& condition,
                      std::chrono::milliseconds deadline)
{
    const auto end = std::chrono::steady_clock::now() + deadline;

    while (!condition() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds {5});

    return condition();
}

inline bool becomesReady(const eacp::Processes::Process& process)
{
    return waitUntil(
        [&]
        {
            return process.output().find(ProcessChildModes::readyLine)
                   != std::string::npos;
        },
        readyDeadline);
}

inline bool exitsWithin(eacp::Processes::Process& process,
                        std::chrono::milliseconds deadline)
{
    return waitUntil([&] { return !process.isRunning(); }, deadline);
}

// Kills a child still running after the deadline, so a failing case never
// blocks in wait().
inline int exitCodeOnceStopped(eacp::Processes::Process& process)
{
    if (!exitsWithin(process, exitDeadline))
        process.kill();

    return process.wait();
}

inline eacp::Processes::ProcessOptions sleeperInItsOwnGroup()
{
    auto options = ProcessChildModes::createChildOptions(
        ProcessChildModes::sleepWhenReady, {"30"});
    options.newProcessGroup = true;
    return options;
}

inline eacp::Processes::ProcessOptions
    stoppableChild(const std::filesystem::path& marker)
{
    return ProcessChildModes::createChildOptions(ProcessChildModes::awaitStopRequest,
                                                 {marker.string()});
}

inline bool stoppedGracefully(const std::filesystem::path& marker)
{
    return std::filesystem::exists(marker)
           && readTestFile(marker) == ProcessChildModes::stoppedMarkerText;
}

inline std::filesystem::path withSuffix(const std::filesystem::path& marker,
                                        const char* suffix)
{
    return marker.string() + suffix;
}

// For a child whose output is not captured: its ready file instead of its
// "ready" line.
inline bool readyFileAppears(const std::filesystem::path& marker)
{
    const auto readyFile = withSuffix(marker, ProcessChildModes::readyFileSuffix);
    return waitUntil([&] { return std::filesystem::exists(readyFile); },
                     readyDeadline);
}

template <typename Result>
bool arrivesWithin(std::future<Result>& result, std::chrono::milliseconds deadline)
{
    return result.wait_for(deadline) == std::future_status::ready;
}
} // namespace ProcessStopping
