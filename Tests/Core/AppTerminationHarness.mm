// Subprocess harness for AppTerminationTests: quits a running app via
// terminate: or Apps::quit(returnValue) and reports whether run<T>()
// unwound. main() returns run<T>()'s value so the parent can check the
// process exit code.
#include <eacp/Core/Core.h>

#import <Cocoa/Cocoa.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace
{
struct TerminatingApp
{
    TerminatingApp()
    {
        eacp::Threads::callAsync([] { [NSApp terminate:nil]; });
    }

    ~TerminatingApp()
    {
        std::puts("app-destroyed");
        std::fflush(stdout);
    }
};

struct QuitWithCodeApp
{
    QuitWithCodeApp() { eacp::Apps::quit(42); }

    ~QuitWithCodeApp()
    {
        std::puts("app-destroyed");
        std::fflush(stdout);
    }
};

int runHarnessApp(int argc, char* argv[])
{
    if (argc > 1 && std::string_view(argv[1]) == "quit-code")
        return eacp::Apps::run<QuitWithCodeApp>();

    return eacp::Apps::run<TerminatingApp>();
}
} // namespace

int main(int argc, char* argv[])
{
    std::thread(
        []
        {
            eacp::Time::sleepMS(15000);
            std::_Exit(3);
        })
        .detach();

    auto exitCode = runHarnessApp(argc, argv);

    std::puts("run-returned");
    std::fflush(stdout);
    return exitCode;
}
