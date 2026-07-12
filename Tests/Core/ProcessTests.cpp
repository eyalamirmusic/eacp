#include "Common.h"
#include <csignal>

#include <sys/types.h>
#include <unistd.h>

using namespace nano;
namespace Proc = eacp::Processes;

auto tCapturesStdout = test("Process/run/capturesStdout") = []
{
    auto result = Proc::run("/bin/echo", {"hello", "world"});

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 0);
    check(result.output == "hello world\n");
    check(result.errorOutput.empty());
};

auto tCapturesStderr = test("Process/run/capturesStderr") = []
{
    auto result = Proc::run("/bin/sh", {"-c", "echo oops 1>&2"});

    check(result.exitCode == 0);
    check(result.errorOutput == "oops\n");
    check(result.output.empty());
};

auto tNonZeroExit = test("Process/run/nonZeroExit") = []
{
    auto result = Proc::run("/bin/sh", {"-c", "exit 7"});

    check(result.launched);
    check(result.exitCode == 7);
};

auto tMissingExecutableExits127 = test("Process/run/missingExecutable") = []
{
    auto result = Proc::run("/no/such/binary-eacp", {});

    // fork() succeeds, execvp() fails, the child exits 127.
    check(result.exitCode == 127);
};

auto tEnvironmentOverride = test("Process/run/environmentOverride") = []
{
    auto options = Proc::ProcessOptions {"/bin/sh",
                                         {"-c", "printf %s \"$EACP_TEST_VAR\""},
                                         {},
                                         {{"EACP_TEST_VAR", "42"}}};

    auto result = Proc::run(std::move(options));

    check(result.output == "42");
};

auto tStdinRoundTrip = test("Process/stdin/roundTrip") = []
{
    auto process = Proc::Process {"/bin/cat", {}};
    check(process.launched());

    check(process.writeToInput("ping\n"));
    process.closeInput();

    auto code = process.wait();

    check(code == 0);
    check(process.output() == "ping\n");
};

auto tTerminateStopsChild = test("Process/terminate/stopsRunningChild") = []
{
    auto process = Proc::Process {"/bin/sleep", {"30"}};
    check(process.launched());
    check(process.isRunning());

    process.terminate();
    auto code = process.wait();

    check(!process.isRunning());
    check(code == 128 + 15); // terminated by SIGTERM
};

auto tRunAsyncResolves = test("Process/runAsync/resolvesOnMainThread") = []
{
    auto async =
        Proc::runAsync(Proc::ProcessOptions {"/bin/echo", {"async"}, {}, {}});

    auto result = async.waitFor(eacp::Time::MS {5000});

    check(result.exitCode == 0);
    check(result.output == "async\n");
};

auto tDetachedSurvivesDestruction = test("Process/detached/survivesDestruction") = []
{
    auto pid = long {-1};

    {
        auto options = Proc::ProcessOptions {"/bin/sleep", {"30"}};
        options.detached = true;

        auto process = Proc::Process {std::move(options)};
        check(process.launched());
        check(process.isRunning());
        pid = process.id();
    }

    // Destroying the Process must leave a detached child running.
    check(::kill((pid_t) pid, 0) == 0);
    ::kill((pid_t) pid, SIGKILL);
};
