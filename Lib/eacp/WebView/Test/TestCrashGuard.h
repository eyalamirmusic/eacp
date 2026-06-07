#pragma once

namespace eacp::WebView::Test
{

// Arms a platform crash guard for process shutdown. On Windows this installs a
// vectored exception handler that swallows the WebView2/WinRT detach-time
// access violation so CTest sees the real test exit code; elsewhere it is a
// no-op. Call once at the top of main().
void installShutdownCrashGuard();

// Records that the test runner has finished and the process is shutting down,
// along with the exit code the guard should report if it intercepts a teardown
// crash. No-op on platforms without a crash guard.
void markTestShutdown(int exitCode);

} // namespace eacp::WebView::Test
