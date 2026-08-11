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

    // Safety net for third-party teardown faults only: swallowing anything
    // before the runner reported its result would hide real failures.
    if (gShuttingDown.load() && code == EXCEPTION_ACCESS_VIOLATION)
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(gExitCode.load()));

    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void installShutdownCrashGuard()
{
    AddVectoredExceptionHandler(1, shutdownAccessViolationFilter);
}

void markTestShutdown(int exitCode)
{
    gExitCode.store(exitCode);
    gShuttingDown.store(true);
}

} // namespace eacp::WebView::Test
