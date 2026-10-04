#include "TestCrashGuard.h"
#include <eacp/Core/Core.h>
#include <eacp/Network/IPC/Lock.h>

#include <cstdlib>
#include <iostream>
#include <memory>

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
//
// Every run except --list-tests first takes a per-user interprocess lock, so
// only one WebView test process runs at a time: under ctest -j several
// browser engines cold-starting at once slowed every case several-fold and
// pushed the first navigation past its timeout.
namespace
{

constexpr auto webViewTestLockName = "com.eacp.webview-tests";
constexpr auto webViewTestLockTimeout = eacp::Time::MS {5 * 60 * 1000};

int gExitCode = 0;

[[noreturn]] void exitWithLockFailure(const std::string& reason)
{
    std::cerr << "WebView tests: cannot take the '" << webViewTestLockName
              << "' lock: " << reason << std::endl;
    std::exit(2);
}

struct ExclusiveRun
{
    ExclusiveRun()
        : lock(makeLock())
        , guard(*lock, webViewTestLockTimeout)
    {
        if (!guard)
            exitWithLockFailure("another WebView test process held it for "
                                "longer than 5 minutes");
    }

    static std::unique_ptr<eacp::IPC::Lock> makeLock()
    {
        try
        {
            return std::make_unique<eacp::IPC::Lock>(webViewTestLockName);
        }
        catch (const eacp::IPC::Error& error)
        {
            exitWithLockFailure(error.what());
        }
    }

    std::unique_ptr<eacp::IPC::Lock> lock;
    eacp::IPC::ScopedLock guard;
};

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
        auto options = parseRunOptions();

        if (!options.listTests)
            exclusiveRun = std::make_unique<ExclusiveRun>();

        gExitCode = nano::run(options);

        // Let the platform crash guard know the real result before WebView2
        // teardown can fault during process shutdown (see TestCrashGuard).
        eacp::WebView::Test::markTestShutdown(gExitCode);
        eacp::Apps::quit();
    }

    std::unique_ptr<ExclusiveRun> exclusiveRun;
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
