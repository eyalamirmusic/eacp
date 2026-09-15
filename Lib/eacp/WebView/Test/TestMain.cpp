#include "TestCrashGuard.h"
#include <eacp/Core/Core.h>

#include <NanoTest/NanoTest.h>

// Prebuilt main() for in-process WebView test binaries (linked via
// eacp-webview-test-main).
//
// Tests run on the same main thread that hosts the WebView's event
// loop. Reusing Apps::run<T> for the entry point gives the tests a
// fully bootstrapped runloop (NSApplication on Apple; the COM
// apartment on Windows) before any test touches a Window or WebView —
// same as a normal app would have.
//
// The TestRunner construction is dispatched onto the runloop's first
// tick by Apps::run<T>; nano::run() blocks the main thread on that
// tick while iterating tests. Per-driver-operation runEventLoopFor()
// calls re-enter the runloop synchronously, returning when the
// matching WebView callback fires stopEventLoop().
namespace
{

auto gExitCode = 0;

// Mirrors NanoTest's argv parsing — same surface, sourced from
// AppEnvironment::commandLineArgs (populated in main()) instead of
// taking argc/argv from a global.
nano::RunOptions parseRunOptions()
{
    auto& args = eacp::Apps::getAppEnvironment().commandLineArgs;
    auto opts = nano::RunOptions {};

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
    // After the run, markTestShutdown lets the platform crash guard
    // know the real result before WebView2 teardown can fault during
    // process shutdown (see TestCrashGuard).
    TestRunner()
    {
        gExitCode = nano::run(parseRunOptions());

        eacp::WebView::Test::markTestShutdown(gExitCode);
        eacp::Apps::quit();
    }
};

} // namespace

// Runs headless, skipping Window's show/activate calls, so test
// binaries can run on CI machines without an active windowing session.
// WebView/JS still functions; only the visible surface is suppressed.
int main(int argc, char* argv[])
{
    eacp::WebView::Test::installShutdownCrashGuard();

    eacp::Apps::getAppEnvironment().headless = true;

    eacp::Apps::run<TestRunner>(argc, argv);
    return gExitCode;
}
