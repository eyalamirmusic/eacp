#pragma once

namespace eacp::WebView::Test
{

// Call once at the top of main(). On Windows, swallows the WebView2/WinRT
// detach-time access violation so CTest sees the real exit code.
void installShutdownCrashGuard();

// Arms the guard, and gives it the exit code to report on a teardown crash.
void markTestShutdown(int exitCode);

} // namespace eacp::WebView::Test
