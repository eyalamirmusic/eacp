#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string_view>

namespace SelfDestruct
{
struct Fault
{
    std::string_view name;
    std::string_view label;
    std::string_view description;
    std::function<void()> trigger;
};

// How long the hang fault sleeps the main thread for.
void setHangDuration(std::chrono::milliseconds duration);

// The faults that stall the main thread rather than crash, which need the
// event loop running, so its heartbeat has started, before they begin.
bool isMainThreadFault(std::string_view name);

// The faults this platform can raise: __fastfail is Windows-only.
const eacp::Vector<Fault>& getFaults();

std::optional<std::reference_wrapper<const Fault>> findFault(std::string_view name);
} // namespace SelfDestruct
