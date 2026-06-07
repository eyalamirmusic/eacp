#include "TestCrashGuard.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <atomic>

namespace eacp::WebView::Test
{
namespace
{
std::atomic<bool> gShuttingDown {false};
std::atomic<int> gExitCode {0};

LONG WINAPI shutdownAccessViolationFilter(EXCEPTION_POINTERS* info)
{
    auto code = info->ExceptionRecord->ExceptionCode;

    // Only swallow access violations that happen after the test runner has
    // reported its result. WebView2 + WinRT teardown deterministically touches
    // freed memory somewhere during DLL detach on this build; skipping the
    // resulting crash lets CTest see the actual exit code the tests produced
    // instead of SEGFAULT.
    if (gShuttingDown.load() && code == EXCEPTION_ACCESS_VIOLATION)
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(gExitCode.load()));

    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void installShutdownCrashGuard()
{
    // SEM_NOGPFAULTERRORBOX is already in effect from eacp-core's static-init
    // in ThreadUtils-Windows.cpp; the vectored handler here is what actually
    // intercepts the WebView2 / WinRT detach access violation and reports the
    // real test exit code.
    AddVectoredExceptionHandler(1, shutdownAccessViolationFilter);
}

void markTestShutdown(int exitCode)
{
    gExitCode.store(exitCode);
    gShuttingDown.store(true);
}

} // namespace eacp::WebView::Test
