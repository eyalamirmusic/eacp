#include "TestCrashGuard.h"
#include <eacp/Core/Core.h>

#include <NanoTest/NanoTest.h>

// Prebuilt main() for in-process WebView test binaries. Going through
// Apps::run<T> gives tests a bootstrapped runloop (NSApplication on Apple,
// the COM apartment on Windows) on the same thread the WebView runs on.
namespace
{

int gExitCode = 0;

nano::RunOptions parseRunOptions()
{
    auto& args = eacp::Apps::getAppEnvironment().commandLineArgs;
    auto opts = nano::RunOptions {};

    // Mirrors NanoTest's own argv parsing.
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

        // Record the real result before WebView2 teardown can fault.
        eacp::WebView::Test::markTestShutdown(gExitCode);
        eacp::Apps::quit();
    }
};

} // namespace

int main(int argc, char* argv[])
{
    eacp::WebView::Test::installShutdownCrashGuard();

    // Suppresses show/activate so CI runners need no windowing session.
    eacp::Apps::getAppEnvironment().headless = true;

    eacp::Apps::run<TestRunner>(argc, argv);
    return gExitCode;
}
