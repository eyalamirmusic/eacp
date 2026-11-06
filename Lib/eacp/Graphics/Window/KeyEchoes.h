#pragma once

/** @file Internal: recognises a forwarded key that a host bounced back in, so it is not
    forwarded twice. */

#include "KeyForwarding.h"

namespace eacp::Graphics
{
// Logic's AUHostingServiceXPC bounces a forwarded key straight back in,
// re-encoded but keeping its timestamp.
bool isEchoOfForwardedKey(const NativeKeyEvent& event);
} // namespace eacp::Graphics
