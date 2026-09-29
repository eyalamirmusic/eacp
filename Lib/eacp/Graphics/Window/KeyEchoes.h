#pragma once

#include "KeyForwarding.h"

namespace eacp::Graphics
{
// Logic's AUHostingServiceXPC bounces a forwarded key straight back in,
// re-encoded but keeping its timestamp.
bool isEchoOfForwardedKey(const NativeKeyEvent& event);
} // namespace eacp::Graphics
