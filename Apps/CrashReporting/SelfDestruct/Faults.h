#pragma once

#include <eacp/Core/Utils/Containers.h>

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

// The faults this platform can raise: __fastfail is Windows-only.
const eacp::Vector<Fault>& getFaults();

std::optional<std::reference_wrapper<const Fault>> findFault(std::string_view name);
} // namespace SelfDestruct
