#include <eacp/Core/App/App.h>
#include <eacp/Core/App/AppEnvironment.h>

#include <NanoTest/NanoTest.h>

#if defined(_WIN32)
    #include <atomic>
    #include <eacp/Core/Utils/WinInclude.h>
#endif

// Prebuilt main() for in-process WebView test binaries (linked via
// eacp-webview-test-main).
//
// Tests run on the same main thread that hosts the WebView's event
// loop. Reusing Apps::run<T> for the entry point gives the tests a
// fully bootstrapped runloop (NSApplication on Apple; WinRT
// apartment + DispatcherQueueController on Windows) before any test
// touches a Window or WebView — same as a normal app would have.
//
// The TestRunner construction is dispatched onto the runloop's first
// tick by Apps::run<T>; nano::run() blocks the main thread on that
// tick while iterating tests. Per-driver-operation runEventLoopFor()
// calls re-enter the runloop synchronously, returning when the
// matching WebView callback fires stopEventLoop().
namespace
{

int gExitCode = 0;

#if defined(_WIN32)
// Track when the test runner has finished so the vectored exception
// handler in main() only intercepts shutdown crashes, not real
// failures during the test itself.
std::atomic<bool> gShuttingDown {false};
#endif

nano::RunOptions parseRunOptions()
{
    auto& args = eacp::Apps::getAppEnvironment().commandLineArgs;
    auto opts = nano::RunOptions {};

    // Mirror NanoTest's argv parsing — same surface, sourced from
    // AppEnvironment::commandLineArgs (populated in main()) instead
    // of taking argc/argv from a global.
    for (auto i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--list-tests")
            opts.listTests = true;
        else if (args[i] == "--test" && i + 1 < args.size())
            opts.test = args[++i];
    }

    return opts;
}

struct TestRunner
{
    TestRunner()
    {
        gExitCode = nano::run(parseRunOptions());
#if defined(_WIN32)
        // Mark shutdown so the vectored exception handler installed in
        // main() will swallow the WebView2 / WinRT detach-time access
        // violation and exit with the real test result.
        gShuttingDown.store(true);
#endif
        eacp::Apps::quit();
    }
};

} // namespace

#if defined(_WIN32)
namespace
{
LONG WINAPI shutdownAccessViolationFilter(EXCEPTION_POINTERS* info)
{
    auto code = info->ExceptionRecord->ExceptionCode;

    // Only swallow access violations that happen after the test runner
    // has reported its result. WebView2 + WinRT teardown deterministically
    // touches freed memory somewhere during DLL detach on this build;
    // skipping the resulting crash lets CTest see the actual exit code
    // the tests produced instead of SEGFAULT.
    if (gShuttingDown.load() && code == EXCEPTION_ACCESS_VIOLATION)
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(gExitCode));

    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace
#endif

int main(int argc, char* argv[])
{
#if defined(_WIN32)
    // SEM_NOGPFAULTERRORBOX is already in effect from eacp-core's
    // static-init in ThreadUtils-Windows.cpp; the vectored handler
    // here is what actually intercepts the WebView2 / WinRT detach
    // access violation and reports the real test exit code.
    AddVectoredExceptionHandler(1, shutdownAccessViolationFilter);
#endif

    // Skip Window's show/activate calls so test binaries can run on
    // CI machines without an active windowing session. WebView/JS
    // still functions; only the visible surface is suppressed.
    eacp::Apps::getAppEnvironment().headless = true;

    eacp::Apps::run<TestRunner>(argc, argv);
    return gExitCode;
}
