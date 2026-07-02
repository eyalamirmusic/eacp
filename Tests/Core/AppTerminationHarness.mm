// Subprocess harness for AppTerminationTests: quits a running app via
// terminate: and reports whether run<T>() unwound.
#include <eacp/Core/App/App.h>

#import <Cocoa/Cocoa.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
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
} // namespace

int main()
{
    std::thread(
        []
        {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            std::_Exit(3);
        })
        .detach();

    eacp::Apps::run<TerminatingApp>();

    std::puts("run-returned");
    std::fflush(stdout);
    return 0;
}
