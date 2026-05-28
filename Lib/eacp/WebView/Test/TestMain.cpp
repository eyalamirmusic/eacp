#include <eacp/Core/App/App.h>

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

int gArgc = 0;
char** gArgv = nullptr;
int gExitCode = 0;

struct TestRunner
{
    TestRunner()
    {
        gExitCode = nano::run(gArgc, gArgv);
        eacp::Apps::quit();
    }
};

} // namespace

int main(int argc, char* argv[])
{
    gArgc = argc;
    gArgv = argv;
    eacp::Apps::run<TestRunner>();
    return gExitCode;
}
