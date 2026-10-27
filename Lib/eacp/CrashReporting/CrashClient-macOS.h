#pragma once

namespace eacp::CrashReporting::Detail
{
// Tells the handler the hang record in the shared block is filled in. False
// with no handler, or once the app is crashing.
bool notifyHandlerOfHang();
} // namespace eacp::CrashReporting::Detail
