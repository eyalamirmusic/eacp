#include <eacp/Core/App/App.h>
#include <eacp/Core/App/AppEnvironment.h>

#include "TestCrashGuard.h"

#include <NanoTest/NanoTest.h>

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

        // Let the platform crash guard know the real result before WebView2 /
        // WinRT teardown can fault during process shutdown (see TestCrashGuard).
        eacp::WebView::Test::markTestShutdown(gExitCode);
        eacp::Apps::quit();
    }
};

} // namespace

int main(int argc, char* argv[])
{
    eacp::WebView::Test::installShutdownCrashGuard();

    // Skip Window's show/activate calls so test binaries can run on
    // CI machines without an active windowing session. WebView/JS
    // still functions; only the visible surface is suppressed.
    eacp::Apps::getAppEnvironment().headless = true;

    eacp::Apps::run<TestRunner>(argc, argv);
    return gExitCode;
}
