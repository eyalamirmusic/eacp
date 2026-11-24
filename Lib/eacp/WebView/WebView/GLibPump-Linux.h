#pragma once

namespace eacp::Graphics::detail
{
// Runs GLib's default main context from eacp's own loop, so WebKit's sources,
// timeouts and idles fire under run(), runFor() and pumpEventLoop() alike. The
// whole context is one descriptor among the loop's sources: an epoll set
// holding GLib's poll fds and a timerfd armed to GLib's next timeout.
//
// Installed on the first call, on the main thread, and never removed. False
// when the main thread could not acquire the context (another thread owns it),
// in which case nothing is pumped.
bool ensureGLibPump();
} // namespace eacp::Graphics::detail
