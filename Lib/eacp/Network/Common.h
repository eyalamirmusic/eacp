#pragma once

#include <eacp/Core/Core.h>
#include <stdexcept>

namespace eacp
{

// loopback is the default: binding 0.0.0.0 makes a host firewall interrupt the
// user to approve the executable. Pass any to actually serve other machines.
enum class BindInterface
{
    loopback,
    any,
};

} // namespace eacp
