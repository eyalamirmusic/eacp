#pragma once

#include <eacp/Core/Core.h>
#include <stdexcept>

namespace eacp
{

// Which interface a listening socket binds to.
//
// loopback is the default: a server nobody asked to publish should not be
// reachable from the network, and binding 0.0.0.0 is what makes a host
// firewall (Windows Defender, say) interrupt the user to approve the
// executable. Pass any to actually serve other machines.
enum class BindInterface
{
    loopback,
    any,
};

} // namespace eacp
