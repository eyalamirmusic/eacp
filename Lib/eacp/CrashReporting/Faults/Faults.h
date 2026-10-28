#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/Time.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string_view>

// Deliberate faults for testing a crash reporter end to end: each one breaks
// the calling process for real, on the main thread or on a background thread.
// For demo and test apps only.
namespace eacp::CrashReporting::Faults
{
enum class ThreadChoice
{
    main,
    background
};

std::string_view getThreadChoiceName(ThreadChoice choice);
std::optional<ThreadChoice> findThreadChoice(std::string_view name);

struct Fault
{
    std::string_view name;
    std::string_view label;
    std::string_view description;
    std::function<void()> trigger = [] {};

    // Stalls its thread rather than crashing: a hang, a spin, a deadlock.
    bool stallsItsThread = false;

    // False where the platform cannot raise it: __fastfail is Windows-only,
    // and an integer divide by zero does not trap on arm64 macOS.
    bool isAvailable = true;
};

// Every fault, available here or not.
const Vector<Fault>& listFaults();

std::optional<std::reference_wrapper<const Fault>> findFault(std::string_view name);

// How long the hang fault sleeps its thread for.
void setHangDuration(std::chrono::milliseconds duration);
std::chrono::milliseconds getHangDuration();

// Raises the fault now: `main` on the calling thread, which should be the
// main one; `background` on a fresh thread that is never joined, and returns
// at once. False when there is no such fault or it is unavailable here.
bool triggerFault(std::string_view name, ThreadChoice thread);

// triggerFault from an event-loop callback `delay` from now. False as above.
bool scheduleFault(std::string_view name, ThreadChoice thread, Time::MS delay);

// Ends the crash handler process, so the next crash finds no handler, goes
// unreported, and the next launch's CrashHandler::unreportedExit sees it.
void killHandlerProcess();
} // namespace eacp::CrashReporting::Faults
